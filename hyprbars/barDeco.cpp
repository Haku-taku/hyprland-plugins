#include "barDeco.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <hyprgraphics/image/Image.hpp>

#include <hyprutils/math/Vector2D.hpp>

#include <dlfcn.h>

#include "hyprland/src/macros.hpp"

#include "globals.hpp"
#include "BarPassElement.hpp"

#include <climits>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) {
    return CHyprColor{static_cast<uint64_t>(color)};
}

float CHyprBar::effectivePaddingScale() const {
    // paddingScale only takes effect when fix_button_center is false
    return g_pGlobalState->config.fixButtonCenter->value() ? 1.0f : m_fPaddingScale->value();
}

float CHyprBar::effectiveButtonSlot(float buttonSize, float btnScale, bool isFirst) const {
    // First button on each side always uses the full slot (fix keeps it in place).
    // Other buttons use full slot only when fix_button_center is true.
    if (isFirst || g_pGlobalState->config.fixButtonCenter->value())
        return buttonSize;
    return buttonSize * btnScale;
}

void executeButtonAction(const SHyprButtonAction& action) {
    // Dispatcher string (e.g. "exec kitty", "workspace +1") takes priority.
    if (!action.dispatcher.empty()) {
        const auto  spc = action.dispatcher.find(' ');
        std::string disp = spc == std::string::npos ? action.dispatcher : action.dispatcher.substr(0, spc);
        std::string args = spc == std::string::npos ? "" : action.dispatcher.substr(spc + 1);
        if (g_pKeybindManager->m_dispatchers.contains(disp))
            g_pKeybindManager->m_dispatchers[disp](args);
        else
            Log::logger->log(Log::ERR, "[hyprbars] unknown dispatcher: {}", disp);
        return;
    }
    // Lua callback or dispatch object.
    if (action.callback == LUA_NOREF || !g_pLuaState)
        return;
    lua_rawgeti(g_pLuaState, LUA_REGISTRYINDEX, action.callback);
    if (lua_isfunction(g_pLuaState, -1)) {
        // Plain function → call it directly
        if (lua_pcall(g_pLuaState, 0, 0, 0) != LUA_OK)
            Log::logger->log(Log::ERR, "[hyprbars] Lua callback error: {}", lua_tostring(g_pLuaState, -1));
    } else {
        // Dispatch object (e.g. hl.dsp.window.close) → wrap with hl.dispatch()
        lua_getglobal(g_pLuaState, "hl");
        lua_getfield(g_pLuaState, -1, "dispatch");
        lua_remove(g_pLuaState, -2);          // remove 'hl' table
        lua_pushvalue(g_pLuaState, -2);       // push the stored object as argument
        lua_remove(g_pLuaState, -3);          // remove original position
        if (lua_pcall(g_pLuaState, 1, 0, 0) != LUA_OK)
            Log::logger->log(Log::ERR, "[hyprbars] dispatch error: {}", lua_tostring(g_pLuaState, -1));
    }
}

static double getOverviewRenderScale() {
    static float* pScale       = nullptr;
    static void*  pOwnerHandle = nullptr;
    static float  lastScale    = 1.0f; // stale but safe if plugin vanishes

    // Check if the cached handle is still a loaded plugin.
    bool cacheValid = false;
    if (pScale) {
        for (const auto* p : g_pPluginSystem->getAllPlugins()) {
            if (p && p->m_handle == pOwnerHandle) {
                cacheValid = true;
                break;
            }
        }
    }

    if (!cacheValid) {
        pScale       = nullptr;
        pOwnerHandle = nullptr;
        for (const auto* p : g_pPluginSystem->getAllPlugins()) {
            if (!p || (!p->m_name.contains("scroll-overview") && !p->m_path.contains("scroll-overview")))
                continue;
            void* sym = dlsym(p->m_handle, "g_fOverviewRenderScale");
            if (sym) {
                pScale       = static_cast<float*>(sym);
                pOwnerHandle = p->m_handle;
                break;
            }
        }
    }

    if (pScale)
        lastScale = *pScale;
    return lastScale;
}

CHyprBar::CHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow = pWindow;

    const auto PMONITOR         = pWindow->m_monitor.lock();
    PMONITOR->m_scheduledRecalc = true;

    m_pAnimConfig = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    m_pAnimConfig->internalEnabled = 1;
    m_pAnimConfig->internalSpeed   = g_pGlobalState->config.animationSpeed->value() / 100.0f;
    m_pAnimConfig->internalBezier  = g_pGlobalState->config.animationBezier->value();
    m_pAnimConfig->pValues         = m_pAnimConfig;

    // button events
    m_pMouseButtonCallback = Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(info, e); });
    m_pTouchDownCallback   = Event::bus()->m_events.input.touch.down.listen([&](ITouch::SDownEvent e, Event::SCallbackInfo& info) { onTouchDown(info, e); });
    m_pTouchUpCallback     = Event::bus()->m_events.input.touch.up.listen([&](ITouch::SUpEvent e, Event::SCallbackInfo& info) { onTouchUp(info, e); });

    // move events
    m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen([&](ITouch::SMotionEvent e, Event::SCallbackInfo& info) { onTouchMove(info, e); });
    m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo& info) { onMouseMove(c); });
    m_pMouseAxisCallback = Event::bus()->m_events.input.mouse.axis.listen([&](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(info, e); });

    g_pAnimationManager->createAnimation(configColor(g_pGlobalState->config.barColor->value()), m_cRealBarColor, m_pAnimConfig,
                                         pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });

    g_pAnimationManager->createAnimation(1.0f, m_fPaddingScale, m_pAnimConfig,
                                         pWindow, AVARDAMAGE_NONE);
    m_fPaddingScale->setUpdateCallback([&](auto) { damageEntire(); });

    m_bWindowHasFocus = pWindow == Desktop::focusState()->window();

    ensureButtonInstances();
    updateButtonStateAnimations();
}

CHyprBar::~CHyprBar() {
    std::erase(g_pGlobalState->bars, m_self);
}

void CHyprBar::ensureButtonInstances() {
    auto pWin = m_pWindow.lock();
    if (!pWin)
        return;

    m_iButtonHoverStateLeft  = 0;
    m_iButtonHoverStateRight = 0;
    m_iButtonPressStateLeft  = 0;
    m_iButtonPressStateRight = 0;
    m_iPressedIdx            = -1;

    const auto& buttons = g_pGlobalState->buttons;

    m_vButtonInstances.clear();
    m_vButtonInstances.reserve(buttons.size());

    for (size_t idx = 0; idx < buttons.size(); ++idx) {
        if (idx >= g_pGlobalState->buttons.size())
            continue;

        const auto& button = g_pGlobalState->buttons[idx];

        SHyprButtonInstance inst;
        inst.buttonIdx         = idx;
        inst.currentState       = 0;
        inst.prevState          = 0;
        inst.needsTextureUpdate = true;

        CHyprColor initBgColor;
        if (button.inactiveBgColor.has_value())
            initBgColor = *button.inactiveBgColor;
        else if (g_pGlobalState->config.inactiveButtonColor->value() != 0)
            initBgColor = configColor(g_pGlobalState->config.inactiveButtonColor->value());
        else
            initBgColor = button.bgcol;

        CHyprColor initIcon        = button.getAutoFgColor(1);
        float      initScale       = button.inactiveScale.value_or(g_pGlobalState->config.inactiveScale->value());
        float      initIconScale   = button.inactiveIconScale.value_or(button.iconScaleDefault);

        g_pAnimationManager->createAnimation(initBgColor, inst.bgColor,
            m_pAnimConfig, pWin, AVARDAMAGE_NONE);
        g_pAnimationManager->createAnimation(initIcon, inst.iconColor,
            m_pAnimConfig, pWin, AVARDAMAGE_NONE);
        g_pAnimationManager->createAnimation(initScale, inst.scale,
            m_pAnimConfig, pWin, AVARDAMAGE_NONE);
        g_pAnimationManager->createAnimation(initIconScale, inst.iconScale,
            m_pAnimConfig, pWin, AVARDAMAGE_NONE);
        g_pAnimationManager->createAnimation(1.0f, inst.crossfadeProgress,
            m_pAnimConfig, pWin, AVARDAMAGE_NONE);
        g_pAnimationManager->createAnimation(0.0f, inst.iconAlpha,
            m_pAnimConfig, pWin, AVARDAMAGE_NONE);

        inst.bgColor->setUpdateCallback([&](auto) { damageEntire(); });
        inst.iconColor->setUpdateCallback([&](auto) { damageEntire(); });
        inst.scale->setUpdateCallback([&](auto) { damageEntire(); });
        inst.iconScale->setUpdateCallback([&](auto) { damageEntire(); });
        inst.crossfadeProgress->setUpdateCallback([&](auto) { damageEntire(); });
        inst.iconAlpha->setUpdateCallback([&](auto) { damageEntire(); });

        m_vButtonInstances.push_back(std::move(inst));
    }
}

void CHyprBar::updateButtonStateAnimations() {

    // Sync with the real focus state — updateWindow() may not be called for every
    // focus change, but updateButtonStateAnimations() runs on mouse move, press,
    // release, and config reload.
    auto pWin = m_pWindow.lock();
    m_bWindowHasFocus = pWin && pWin == Desktop::focusState()->window();

    const bool EFFECTIVE_MOUSE_ON_BAR = m_bMouseOnBar || (m_iPressedIdx >= 0);
    const bool EFFECTIVE_WINDOW_FOCUS = m_bWindowHasFocus || (m_iPressedIdx >= 0);

    // Animate padding scale: 1.0 when active/focused, config value when inactive.
    *m_fPaddingScale = EFFECTIVE_WINDOW_FOCUS ? 1.0f : g_pGlobalState->config.inactivePadScale->value();

    auto updateList = [&](std::vector<size_t>& indices,
                          unsigned int hoverBits, unsigned int pressBits) {
        for (size_t i = 0; i < indices.size(); ++i) {
            auto&       inst     = m_vButtonInstances[indices[i]];
            const auto& button   = g_pGlobalState->buttons[inst.buttonIdx];

            int newState;
            if (!EFFECTIVE_WINDOW_FOCUS)
                newState = 0;                  // inactive
            else if (pressBits & (1 << i))
                newState = 3;                  // button-pressed (takes priority over focus)
            else if (hoverBits & (1 << i))
                newState = 2;                  // button-hovered
            else
                newState = 1;                  // active

            // Without focus: all icons hidden. Pressed: always visible.
            // States 1 (active) / 2 (hovered) follow icon_on_hover mode:
            //   0 = always show when focused
            //   1 = show when mouse is anywhere on the bar
            //   2 = show only when hovering the specific button
            const int  ICON_MODE              = g_pGlobalState->config.iconOnHover->value();
            float      targetIconAlpha;
            if (!EFFECTIVE_WINDOW_FOCUS)
                targetIconAlpha = 0.0f;
            else if (newState == 3)
                targetIconAlpha = 1.0f;
            else if (ICON_MODE == 1 && !EFFECTIVE_MOUSE_ON_BAR)
                targetIconAlpha = 0.0f;
            else if (ICON_MODE == 2 && newState != 2)
                targetIconAlpha = 0.0f;
            else
                targetIconAlpha = 1.0f;
            if (targetIconAlpha != inst.iconAlpha->goal())
                *inst.iconAlpha = targetIconAlpha;

            if (newState == inst.currentState && !inst.needsTextureUpdate)
                continue;

            int oldState     = inst.currentState;
            inst.currentState = newState;

            // Determine targets based on new state
            CHyprColor targetBgColor;
            float      targetCircleScale;
            float      targetIconScale;

            switch (newState) {
                case 0: // inactive
                    if (button.inactiveBgColor.has_value())
                        targetBgColor = *button.inactiveBgColor;
                    else if (g_pGlobalState->config.inactiveButtonColor->value() != 0)
                        targetBgColor = configColor(g_pGlobalState->config.inactiveButtonColor->value());
                    else
                        targetBgColor = button.bgcol;
                    targetCircleScale = button.inactiveScale.value_or(g_pGlobalState->config.inactiveScale->value());
                    targetIconScale   = button.inactiveIconScale.value_or(inst.iconScale->value());
                    break;
                case 1: // active
                    targetBgColor     = button.bgcol;
                    targetCircleScale = 1.0f;
                    targetIconScale   = 1.0f;
                    break;
                case 2: // button-hovered
                    targetBgColor     = button.getBgColor(2);
                    targetCircleScale = button.getScale(2);
                    targetIconScale   = button.getIconScale(2);
                    break;
                case 3: // button-pressed
                    targetBgColor     = button.getBgColor(3);
                    targetCircleScale = button.getScale(3);
                    targetIconScale   = button.getIconScale(3);
                    break;
            }

            if (g_pGlobalState->config.inactiveButtonColor->value() > 0 && !m_bWindowHasFocus)
                targetBgColor = configColor(g_pGlobalState->config.inactiveButtonColor->value());

            CHyprColor targetIcon = button.getAutoFgColor(newState);
            if (targetBgColor != inst.bgColor->goal())
                *inst.bgColor = targetBgColor;
            if (targetIcon != inst.iconColor->goal())
                *inst.iconColor = targetIcon;
            if (targetCircleScale != inst.scale->goal())
                *inst.scale = targetCircleScale;
            if (targetIconScale != inst.iconScale->goal())
                *inst.iconScale = targetIconScale;

            // Trigger crossfade when state changes
            if (oldState != newState) {
                inst.needsTextureUpdate = true;
                inst.prevState = oldState;
                inst.crossfadeProgress->setValueAndWarp(0.0f);
                *inst.crossfadeProgress = 1.0f;
            }
        }
    };

    updateList(g_pGlobalState->buttonsLeft,  m_iButtonHoverStateLeft,  m_iButtonPressStateLeft);
    updateList(g_pGlobalState->buttonsRight, m_iButtonHoverStateRight, m_iButtonPressStateRight);
}

void CHyprBar::invalidateButtonTextures() {
    for (auto& inst : m_vButtonInstances) {
        inst.needsTextureUpdate = true;
        inst.cachedButtonSize   = 0.0f;
        inst.texDefault = nullptr;
        inst.texHover   = nullptr;
        inst.texPress   = nullptr;
    }
}

SP<Render::ITexture> CHyprBar::loadSvgIcon(const std::string& path, float pxSize) {
    if (path.empty())
        return nullptr;

    try {
        Hyprgraphics::CImage svgImage(path, Hyprutils::Math::Vector2D{pxSize, pxSize});
        if (!svgImage.success()) {
            Log::logger->log(Log::ERR, "[hyprbars] Failed to load SVG '{}': {}", path, svgImage.getError());
            return nullptr;
        }
        auto cairoSurf = svgImage.cairoSurface();
        if (!cairoSurf || !cairoSurf->cairo()) {
            Log::logger->log(Log::ERR, "[hyprbars] Failed to get cairo surface from SVG '{}'", path);
            return nullptr;
        }
        return g_pHyprRenderer->createTexture(cairoSurf->cairo());
    } catch (std::exception& e) {
        Log::logger->log(Log::ERR, "[hyprbars] Exception loading SVG '{}': {}", path, e.what());
        return nullptr;
    }
}

static bool isSvgPath(const std::string& str) {
    return str.ends_with(".svg") || str.ends_with(".svg.gz");
}

SP<Render::ITexture> CHyprBar::getOrCreateIconTexture(SHyprButtonInstance& inst, int state, float buttonSize, CHyprColor col) {
    const auto& button = g_pGlobalState->buttons[inst.buttonIdx];

    std::string iconStr = button.getIcon(state);
    if (iconStr.empty())
        return nullptr;
    bool svg = isSvgPath(iconStr);

    if (svg)
        return loadSvgIcon(iconStr, buttonSize);

    CHyprColor fg = button.getAutoFgColor(state);
    return g_pHyprRenderer->renderText(iconStr, col,
        std::round(buttonSize * g_pGlobalState->config.iconScale->value()), false, g_pGlobalState->config.barTextFont->value(),
        buttonSize);
}

SDecorationPositioningInfo CHyprBar::getPositioningInfo() {
    const auto                 HEIGHT     = g_pGlobalState->config.barHeight->value();
    const auto                 ENABLED    = g_pGlobalState->config.enabled->value();
    const auto                 PRECEDENCE = g_pGlobalState->config.barPrecedenceOverBorder->value();

    SDecorationPositioningInfo info;
    info.policy         = m_hidden ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
    info.edges          = DECORATION_EDGE_TOP;
    info.priority       = PRECEDENCE ? 10005 : 5000;
    info.reserved       = true;
    info.desiredExtents = {{0, m_hidden || !ENABLED ? 0 : HEIGHT}, {0, 0}};
    return info;
}

void CHyprBar::onPositioningReply(const SDecorationPositioningReply& reply) {
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
        m_bWindowSizeChanged = true;

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CHyprBar::getDisplayName() {
    return "Hyprbar";
}

bool CHyprBar::inputIsValid() {
    if (!g_pGlobalState->config.enabled->value())
        return false;

    if (!m_pWindow->m_workspace || !m_pWindow->m_workspace->isVisible() || !g_pInputManager->m_exclusiveLSes.empty() ||
        (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource())))
        return false;

    const auto WINDOWATCURSOR = g_pCompositor->vectorToWindowUnified(g_pInputManager->getMouseCoordsInternal(),
                                                                     Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto       focusState = Desktop::focusState();
    auto       window     = focusState->window();
    auto       monitor    = focusState->monitor();

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != window)
        return false;

    // check if input is on top or overlay shell layers
    auto     PMONITOR     = monitor;
    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    // check top layer
    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);

    if (foundSurface)
        return false;
    // check overlay layer
    g_pCompositor->vectorToLayerSurface(g_pInputManager->getMouseCoordsInternal(), &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords,
                                        &foundSurface);

    if (foundSurface)
        return false;

    return true;
}

void CHyprBar::onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (m_bRendering)
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        // Always process release — Wayland's implicit grab delivers it to the
        // surface that received the press, even if the cursor left the window.
        handleUpEvent(info);
        return;
    }

    // Only validate on press
    if (!inputIsValid())
        return;

    // Track which physical button was pressed for callback dispatch on release
    m_iPressedButton = e.button;

    handleDownEvent(info, std::nullopt);
}

void CHyprBar::onMouseAxis(Event::SCallbackInfo& info, IPointer::SAxisEvent e) {
    if (m_bRendering || !inputIsValid() || e.source != WL_POINTER_AXIS_SOURCE_WHEEL)
        return;

    // Find which button (if any) the cursor is over and fire the scroll callback
    const auto COORDS           = cursorRelativeToBar();
    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUF           = Vector2D{(int)assignedBoxGlobal().w, (int)HEIGHT};

    if (!VECINRECT(COORDS, 0, 0, BARBUF.x, BARBUF.y - 1))
        return;

    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto SCALEPADDING     = effectivePaddingScale();

    auto checkSide = [&](const std::vector<size_t>& indices, bool isRight) -> const SHyprButtonAction* {
        float offset = BARPADDING;
        for (size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] >= g_pGlobalState->buttons.size()) continue;
            const auto& button = g_pGlobalState->buttons[indices[i]];
            const float centerX = isRight ? (BARBUF.x - offset - button.size / 2.0f)
                                          : (offset + button.size / 2.0f);
            const float centerY = BARBUF.y / 2.0f;
            const double distSq = std::pow(COORDS.x - centerX, 2) + std::pow(COORDS.y - centerY, 2);
            if (distSq <= std::pow(button.size / 2.0f, 2)) {
                if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                    return e.relativeDirection == WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL
                               ? &button.scrollDown : &button.scrollUp;
                else if (e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
                    return &button.scrollClick;
            }
            offset += button.size + BARBUTTONPADDING * SCALEPADDING;
        }
        return nullptr;
    };

    auto action = checkSide(g_pGlobalState->buttonsLeft, false);
    if (!action)
        action = checkSide(g_pGlobalState->buttonsRight, true);

    if (action)
        executeButtonAction(*action);
}

void CHyprBar::onTouchDown(Event::SCallbackInfo& info, ITouch::SDownEvent e) {
    // Don't do anything if you're already grabbed a window with another finger
    if (m_bRendering || !inputIsValid() || e.touchID != 0)
        return;

    handleDownEvent(info, e);
}

void CHyprBar::onTouchUp(Event::SCallbackInfo& info, ITouch::SUpEvent e) {
    if (!m_bDragPending || !m_bTouchEv || e.touchID != m_touchId)
        return;

    handleUpEvent(info);
}

void CHyprBar::onMouseMove(Vector2D coords) {
    // ensure proper redraws of button icons on hover when using hardware cursors
    // if (g_pGlobalState->config.iconOnHover->value())
        damageOnButtonHover();

    if (m_bRendering)
        return;

    if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
        return;

    m_bDragPending = false;
    handleMovement();
}

void CHyprBar::onTouchMove(Event::SCallbackInfo& info, ITouch::SMotionEvent e) {
    if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
        return;

    auto PMONITOR     = m_pWindow->m_monitor.lock();
    PMONITOR          = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
    const auto COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y);

    if (!m_bDraggingThis) {
        // Initial setup for dragging a window.
        g_pKeybindManager->m_dispatchers["setfloating"]("activewindow");
        g_pKeybindManager->m_dispatchers["resizewindowpixel"]("exact 50% 50%,activewindow");
        // pin it so you can change workspaces while dragging a window
        g_pKeybindManager->m_dispatchers["pin"]("activewindow");
    }
    g_pKeybindManager->m_dispatchers["movewindowpixel"](std::format("exact {} {},activewindow", (int)(COORDS.x - (assignedBoxGlobal().w / 2)), (int)COORDS.y));
    m_bDraggingThis = true;
}

void CHyprBar::handleDownEvent(Event::SCallbackInfo& info, std::optional<ITouch::SDownEvent> touchEvent) {
    m_bTouchEv = touchEvent.has_value();
    if (m_bTouchEv)
        m_touchId = touchEvent.value().touchID;

    const auto PWINDOW = m_pWindow.lock();

    auto       COORDS = cursorRelativeToBar();
    if (m_bTouchEv) {
        ITouch::SDownEvent e        = touchEvent.value();
        PHLMONITOR PMONITOR = nullptr;
        for(auto& m : State::monitorState()->monitors()) {
            if(m->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
                PMONITOR = m;
                break;
            }
        }
        PMONITOR                    = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
        COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y) - assignedBoxGlobal().pos();
    }

    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ON_DOUBLE_CLICK  = g_pGlobalState->config.onDoubleClick->value();

    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, HEIGHT - 1)) {

        if (m_bDraggingThis) {
            if (m_bTouchEv)
                g_pKeybindManager->m_dispatchers["settiled"]("activewindow");
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            Log::logger->log(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        m_bTouchEv      = false;
        if (m_iPressedIdx >= 0) {
            m_iButtonPressStateLeft  = 0;
            m_iButtonPressStateRight = 0;
            m_iPressedIdx      = -1;
            updateButtonStateAnimations();
        }
        return;
    }

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        g_pCompositor->changeWindowZOrder(PWINDOW, true);

    info.cancelled   = true;
    m_bCancelledDown = true;

    if (doButtonPress(BARPADDING, BARBUTTONPADDING, HEIGHT, COORDS))
        return;

    if (!ON_DOUBLE_CLICK.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
        Config::Supplementary::executor()->spawn(ON_DOUBLE_CLICK);
        m_bDragPending = false;
    } else {
        m_lastMouseDown = Time::steadyNow();
        m_bDragPending  = true;
    }
}

void CHyprBar::handleUpEvent(Event::SCallbackInfo& info) {
    // Don't check window focus here — Wayland's implicit grab delivers the release
    // to the same surface that received the press, even if focus changed.

    if (m_bCancelledDown)
        info.cancelled = true;

    if (m_iPressedIdx >= 0) {
        const auto COORDS           = cursorRelativeToBar();
        const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
        const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
        const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
        const auto SCALEPADDING     = effectivePaddingScale();
        const auto BARBUF           = Vector2D{(int)assignedBoxGlobal().w, (int)HEIGHT};

        const auto& indices   = m_bPressedIsRight ? g_pGlobalState->buttonsRight : g_pGlobalState->buttonsLeft;
        if (m_iPressedIdx < static_cast<int>(indices.size()) && indices[m_iPressedIdx] < g_pGlobalState->buttons.size()) {
            const auto& button = g_pGlobalState->buttons[indices[m_iPressedIdx]];

            // Reconstruct the button's center position (logical pixels), matching
            // doButtonPress and damageOnButtonHover.
            float offset = BARPADDING;
            for (int j = 0; j < m_iPressedIdx; ++j) {
                if (indices[j] < g_pGlobalState->buttons.size()) {
                    const float scale    = m_vButtonInstances[j].scale->value();
                    const float slotSize = effectiveButtonSlot(g_pGlobalState->buttons[indices[j]].size, scale, j == 0);
                    offset += slotSize + BARBUTTONPADDING * SCALEPADDING;
                }
            }

            const float  centerX   = m_bPressedIsRight ? (BARBUF.x - offset - button.size / 2.0f)
                                                       : (offset + button.size / 2.0f);
            const float  centerY   = BARBUF.y / 2.0f;
            const double distSq    = std::pow(COORDS.x - centerX, 2) + std::pow(COORDS.y - centerY, 2);
            const double radiusSq  = std::pow(button.size / 2.0f, 2);

            if (distSq <= radiusSq) {
                // Release within the same button → dispatch to the correct action
                const SHyprButtonAction* action = nullptr;
                switch (m_iPressedButton) {
                    case 0x110: action = &button.click; break;        // BTN_LEFT
                    case 0x111: action = &button.rightClick; break;   // BTN_RIGHT
                    case 0x112: action = &button.middleClick; break;  // BTN_MIDDLE
                }
                if (action)
                    executeButtonAction(*action);
            }
        }

        m_iButtonPressStateLeft  = 0;
        m_iButtonPressStateRight = 0;
        m_iPressedIdx      = -1;
        updateButtonStateAnimations();
    }

    // Always clean up drag state on every release — whether a button was pressed
    // or the bar itself was clicked for dragging.
    m_bDragPending   = false;
    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        if (m_bTouchEv)
            (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);

        Log::logger->log(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;
    m_bTouchEv     = false;
    m_touchId      = 0;
}

void CHyprBar::handleMovement() {
    g_pKeybindManager->changeMouseBindMode(MBIND_MOVE);
    m_bDraggingThis = true;
    Log::logger->log(Log::DEBUG, "[hyprbars] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
    return;
}

bool CHyprBar::doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS) {
    const auto SCALEPADDING = effectivePaddingScale();

    auto checkList = [&](const std::vector<size_t>& indices,
                         bool BUTTONSRIGHT, unsigned int& pressBits, unsigned int& otherPressBits,
                         unsigned int& otherHoverBits, int& pressedIdx, bool& pressedIsRight) -> bool {
        const Vector2D BARBUF = {(int)assignedBoxGlobal().w, barHeight};
        float          offset = barPadding;

        for (size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] >= g_pGlobalState->buttons.size()) continue;
            const auto& button   = g_pGlobalState->buttons[indices[i]];
            const float btnScale  = m_vButtonInstances[indices[i]].scale->value();
            const float slotSize  = effectiveButtonSlot(button.size, btnScale, i == 0);

            const float centerX   = BUTTONSRIGHT ? (BARBUF.x - offset - slotSize / 2.0f)
                                                 : (offset + slotSize / 2.0f);
            const float centerY   = BARBUF.y / 2.0f;

            const double distSq   = std::pow(COORDS.x - centerX, 2) + std::pow(COORDS.y - centerY, 2);
            const double radiusSq = std::pow(button.size / 2.0f, 2);

            if (distSq <= radiusSq) {
                pressBits |= (1 << i);
                otherPressBits  = 0;
                otherHoverBits  = 0;
                pressedIdx      = static_cast<int>(i);
                pressedIsRight  = BUTTONSRIGHT;
                updateButtonStateAnimations();
                return true;
            }

            offset += slotSize + barButtonPadding * SCALEPADDING;
        }
        return false;
    };

    if (checkList(g_pGlobalState->buttonsRight, true, m_iButtonPressStateRight,
                  m_iButtonPressStateLeft, m_iButtonHoverStateLeft,
                  m_iPressedIdx, m_bPressedIsRight))
        return true;

    if (checkList(g_pGlobalState->buttonsLeft, false, m_iButtonPressStateLeft,
                  m_iButtonPressStateRight, m_iButtonHoverStateRight,
                  m_iPressedIdx, m_bPressedIsRight))
        return true;

    return false;
}

void CHyprBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    const auto COLORVAL         = g_pGlobalState->config.textColor->value();
    const auto SIZE             = g_pGlobalState->config.barTextSize->value();
    const auto WEIGHT           = g_pGlobalState->config.barTextWeight->value();
    const auto FONT             = g_pGlobalState->config.barTextFont->value();
    const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    auto computeSide = [&](const std::vector<size_t>& indices) -> float {
        auto total = 0.0;
        if (!indices.empty()) total = -BARBUTTONPADDING;
        for (auto& idx : indices) {
            total += BARBUTTONPADDING;
            total += g_pGlobalState->buttons[idx].size;
        }
        return total;
    };

    const auto scaledSize        = SIZE * scale;
    const auto sizesL            = computeSide(g_pGlobalState->buttonsLeft);
    const auto sizesR            = computeSide(g_pGlobalState->buttonsRight);
    const auto buttonSizes       = ALIGN == "center" ? std::max(sizesL, sizesR) * 2 :
                                                       sizesL + sizesR;

    const auto scaledButtonsSize = buttonSizes * scale;
    const auto scaledBarPadding  = BARPADDING * scale;
    const auto scaledBorderSize  = getOwner()->getRealBorderSize() * scale;
    const auto alignGap          = (ALIGN == "left" || ALIGN == "right") ? 4 * scale : 0;
    const int  paddingTotal      = scaledBarPadding * 2 + scaledButtonsSize + alignGap;
    const auto maxWidth          = std::clamp((bufferSize.x - paddingTotal), 0.0, bufferSize.x);

    if (m_szLastTitle.empty() || maxWidth < scaledSize * 2 /* too narrow even for "…" */) {
        m_pTextTex = nullptr;
        return;
    }

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(configColor(COLORVAL));
    // m_pTextTex             = g_pHyprRenderer->renderText(m_szLastTitle, COLOR, scaledSize, false, FONT, maxWidth, WEIGHT.m_value);

    m_pTextTex = g_pHyprRenderer->renderText(
        Hyprgraphics::CTextResource::STextResourceData{.text      = m_szLastTitle,
                                                       .font      = FONT,
                                                       .fontSize  = std::round(scaledSize),
                                                       .color     = Hyprgraphics::CColor{Hyprgraphics::CColor::SSRGB{COLOR.r, COLOR.g, COLOR.b}},
                                                       .align     = ALIGN == "left"  ? Hyprgraphics::CTextResource::TEXT_ALIGN_LEFT :
                                                                    ALIGN == "right" ? Hyprgraphics::CTextResource::TEXT_ALIGN_RIGHT :
                                                                                       Hyprgraphics::CTextResource::TEXT_ALIGN_CENTER,
                                                       .maxSize   = Hyprutils::Math::Vector2D{maxWidth, scaledSize}.round(),
                                                       .antialias = CAIRO_ANTIALIAS_GOOD,
                                                       .hintStyle = CAIRO_HINT_STYLE_SLIGHT,
                                                       .ellipsize = true,
                                                       .wrap      = false,
                                                       .weight    = WEIGHT.m_value,
                                                       .italic    = false,
    });
}

size_t CHyprBar::getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D& bufferSize, const float scale) {
    const auto SCALEPADDING = m_fPaddingScale->value();
    float      availableSpace = bufferSize.x - barPadding * scale * 2;
    size_t     count          = 0;

    for (const auto& button : g_pGlobalState->buttons) {
        const float buttonSpace = (button.size + barButtonPadding * SCALEPADDING) * scale;
        if (availableSpace >= buttonSpace) {
            count++;
            availableSpace -= buttonSpace;
        } else
            break;
    }

    return count;
}

void CHyprBar::renderBarButtons(CBox* barBox, const float scale, const float a) {
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto FIXCENTER        = g_pGlobalState->config.fixButtonCenter->value();
    const auto SCALEPADDING     = effectivePaddingScale();

    auto getTexForState = [](SHyprButtonInstance& inst, int s) -> SP<Render::ITexture> {
        switch (s) {
            case 0:                            // inactive: default icon (alpha controls visibility)
            case 1: return inst.texDefault;    // bar-hovered: default icon
            case 2: return inst.texHover;      // button-hovered: hover icon
            case 3: return inst.texPress;      // pressed: press icon
            default: return nullptr;
        }
    };

    auto renderIcon = [&](SP<Render::ITexture> tex, float alpha, float scale,
                          float centerX) {
        if (!tex || tex->m_texID == 0 || alpha <= 0.0f)
            return;
        float texW = tex->m_size.x * scale;
        float texH = tex->m_size.y * scale;
        const auto iconX = centerX - texW / 2.0;
        const auto iconY = barBox->y + barBox->height / 2.0 - texH / 2.0;
        g_pHyprOpenGL->renderTexture(tex, {iconX, iconY, texW, texH}, {.a = alpha});
    };

    auto renderSide = [&](std::vector<size_t>& indices, bool BUTTONSRIGHT) {
    const auto visibleCount = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    float      totalOffset  = BARPADDING * scale;
    const auto COORDS       = cursorRelativeToBar();

    size_t nth = 0;
    for (auto& idx :indices) {
        auto&      button           = g_pGlobalState->buttons[idx];
        auto&      inst             = m_vButtonInstances[idx];

        float      btnScale         = inst.scale->value();
        float      iconScale        = inst.iconScale->value() * btnScale;
        const auto scaledButtonSize = button.size * scale * btnScale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale * SCALEPADDING;

        auto       color            = inst.bgColor->value();
        int        state            = inst.currentState;
        int        prevState        = inst.prevState;
        float      iconAlpha        = inst.iconAlpha->value();
        float      crossfade        = inst.crossfadeProgress->value();

        bool       crossfading      = crossfade < 1.0f && prevState != state;

        color.a *= a;

        float OFFSET    = (1 - btnScale) * button.size * scale;
        float offset    = (FIXCENTER || nth == 0) ? OFFSET : 0.0f;
        CBox  buttonBox = {barBox->x + (BUTTONSRIGHT ? barBox->w - totalOffset - offset / 2.0 - scaledButtonSize : totalOffset + offset / 2.0),
                           barBox->y + (barBox->h - scaledButtonSize) / 2.0, scaledButtonSize, scaledButtonSize};
        buttonBox.round();

        g_pHyprOpenGL->renderRect(buttonBox, color, {.round = static_cast<int>(std::round(scaledButtonSize / 2.0)), .roundingPower = 2.F});

        if (scaledButtonSize != inst.cachedButtonSize) {
            inst.needsTextureUpdate = true;
            inst.cachedButtonSize   = scaledButtonSize;
        }
        if (inst.needsTextureUpdate) {
            inst.texDefault = getOrCreateIconTexture(inst, 1, scaledButtonSize, button.getFgColor(1));  // default icon (states 0 & 1)
            inst.texHover   = getOrCreateIconTexture(inst, 2, scaledButtonSize, button.getFgColor(2));  // hover icon (state 2)
            inst.texPress   = getOrCreateIconTexture(inst, 3, scaledButtonSize, button.getFgColor(3));  // press icon (state 3)
            inst.needsTextureUpdate = false;
        }

        float centerX = buttonBox.x + scaledButtonSize / 2.0;
        if (crossfading) {
            SP<Render::ITexture> texOld = getTexForState(inst, prevState);
            SP<Render::ITexture> texNew = getTexForState(inst, state);

            if (texOld) renderIcon(texOld, a * iconAlpha * (1.0f - crossfade), iconScale, centerX);
            if (texNew) renderIcon(texNew, a * iconAlpha * crossfade,          iconScale, centerX);
        } else {
            SP<Render::ITexture> tex = getTexForState(inst, state);
            if (tex)
                renderIcon(tex, a * iconAlpha, iconScale, centerX);
        }

        totalOffset += scaledButtonsPad + scaledButtonSize + (FIXCENTER ? OFFSET : 0.0f);
        ++nth;
    }
    };

    renderSide(g_pGlobalState->buttonsLeft,  false);
    renderSide(g_pGlobalState->buttonsRight, true);
}

void CHyprBar::draw(PHLMONITOR pMonitor, const float& a) {
    const auto ENABLED = g_pGlobalState->config.enabled->value();

    if (m_bLastEnabledState != ENABLED) {
        m_bLastEnabledState = ENABLED;
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (m_hidden || !validMapped(m_pWindow) || !ENABLED)
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW) return;
    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    if (m_vButtonInstances.size() != g_pGlobalState->buttons.size())
        ensureButtonInstances();

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

void CHyprBar::renderPass(PHLMONITOR pMonitor, const float& a) {
    if (m_bRendering) return;
    m_bRendering = true;
    Hyprutils::Utils::CScopeGuard guard([this] { m_bRendering = false; });

    if (!pMonitor || pMonitor->m_transformedSize.x <= 0 || pMonitor->m_transformedSize.y <= 0)
        return;

    const auto  PWINDOW = m_pWindow.lock();
    if (!PWINDOW) return;

    if (!PWINDOW->m_workspace) return;

    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
    const auto  BARCOLOR          = g_pGlobalState->config.barColor->value();
    const auto  HEIGHT            = g_pGlobalState->config.barHeight->value();
    const auto  PRECEDENCE        = g_pGlobalState->config.barPrecedenceOverBorder->value();
    const auto  ENABLETITLE       = g_pGlobalState->config.barTitleEnabled->value();
    const auto  ENABLEBLUR        = g_pGlobalState->config.barBlur->value();
    const auto  INACTIVECOLOR     = g_pGlobalState->config.inactiveButtonColor->value();

    if (INACTIVECOLOR > 0) {
        bool currentWindowFocus = PWINDOW == Desktop::focusState()->window();
        if (currentWindowFocus != m_bWindowHasFocus) {
            m_bWindowHasFocus = currentWindowFocus;
            updateButtonStateAnimations();
        }
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(configColor(BARCOLOR));
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    color.a *= a;
    const bool SHOULDBLUR   = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F;

    if (HEIGHT < 1) {
        m_iLastHeight = HEIGHT;
        return;
    }

    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    const auto ROUNDING = PWINDOW->rounding() + (PRECEDENCE ? 0 : PWINDOW->getRealBorderSize());

    const auto overviewScale = getOverviewRenderScale();
    const auto renderScale   = pMonitor->m_scale * overviewScale; // combined monitor + overview scale for sub-renderers

    const auto scaledRounding = ROUNDING > 0 ? ROUNDING * renderScale - 2 /* idk why but otherwise it looks bad due to the gaps */ : 0;

    m_seExtents = {{0, HEIGHT}, {}};

    const auto DECOBOX = assignedBoxGlobal();

    const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

    CBox       titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w,
                              DECOBOX.h + scaledRounding * 2 + 1 /* to fill the bottom cuz we can't disable rounding there */};

    titleBarBox.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();

    if (titleBarBox.w < 1 || titleBarBox.h < 1)
        return;

    g_pHyprOpenGL->scissor(titleBarBox);

    if (ROUNDING) {
        // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
        CBox windowBox = {PWINDOW->m_realPosition->value().x + PWINDOW->m_floatingOffset.x - pMonitor->m_position.x + 1,
                          PWINDOW->m_realPosition->value().y + PWINDOW->m_floatingOffset.y - pMonitor->m_position.y + 1, PWINDOW->m_realSize->value().x - 2,
                          PWINDOW->m_realSize->value().y - 2};

        if (windowBox.w < 1 || windowBox.h < 1)
            return;

        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);

        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        windowBox.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(windowBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_NOTEQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    if (SHOULDBLUR)
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower(), .blur = true, .blurA = a});
    else
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    // render title
    if (ENABLETITLE && (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || !m_pTextTex || m_pTextTex->m_texID == 0 || m_bTitleColorChanged)) {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(BARBUF, renderScale);
    }

    if (ROUNDING) {
        // cleanup stencil
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }

    CBox textBox = {titleBarBox.x, titleBarBox.y, (int)BARBUF.x, (int)BARBUF.y};
    if (ENABLETITLE && m_pTextTex && m_pTextTex->m_size.x > 0 && m_pTextTex->m_size.y > 0) {
        const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
        const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
        const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();

        const auto& buttons = g_pGlobalState->buttons;
        auto sideSize = [&](std::vector<size_t>& indices) -> float {
            float t = 0.0;
            if (!indices.empty()) t = -BARBUTTONPADDING;
            for (auto& idx: indices) {
                t += buttons[idx].size + BARBUTTONPADDING;
            }
            return t;
        };

        const auto scaledBorderSize  = PWINDOW->getRealBorderSize() * pMonitor->m_scale;
        const auto scaledButtonsSzL  = sideSize(g_pGlobalState->buttonsLeft) * renderScale;
        const auto scaledButtonsSzR  = sideSize(g_pGlobalState->buttonsRight) * renderScale;
        const auto scaledBarPadding  = BARPADDING * renderScale;

        const auto alignGap          = (ALIGN == "left" || ALIGN == "right") ? static_cast<int>(4 * renderScale) : 0;
        const auto xOffset           = ALIGN == "left" ? std::round(scaledBarPadding + scaledButtonsSzL + alignGap) :
                                       ALIGN == "right"? std::round(BARBUF.x - scaledBarPadding - scaledButtonsSzR - alignGap - m_pTextTex->m_size.x) :
                                       ALIGN == "auto" ? std::round((BARBUF.x + scaledButtonsSzL - scaledButtonsSzR - m_pTextTex->m_size.x) / 2.0) :
                                                         std::round((BARBUF.x - m_pTextTex->m_size.x) / 2.0);
        const auto yOffset           = std::round((BARBUF.y - m_pTextTex->m_size.y) / 2.0);
        CBox       titleBox          = {textBox.x + xOffset, textBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

        Log::logger->log(Log::DEBUG, "texW={} texH={} boxX={} boxY={}", m_pTextTex->m_size.x, m_pTextTex->m_size.y > 0, titleBox.x, titleBox.y);
        g_pHyprOpenGL->renderTexture(m_pTextTex, titleBox, {.a = a});
    }

    renderBarButtons(&textBox, renderScale, a);
    m_bButtonsDirty = false;

    g_pHyprOpenGL->scissor(nullptr);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;

    // dynamic updates change the extents
    if (m_fLastScale != renderScale) {
        PWINDOW->layoutTarget()->recalc();
        m_fLastScale = renderScale;
    }
}

eDecorationType CHyprBar::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CHyprBar::updateWindow(PHLWINDOW pWindow) {
    bool focused = pWindow == Desktop::focusState()->window();
    if (focused != m_bWindowHasFocus) {
        m_bWindowHasFocus = focused;
        updateButtonStateAnimations();
    }
    damageEntire();
}

void CHyprBar::onConfigReloaded() {
    m_pAnimConfig->internalSpeed  = g_pGlobalState->config.animationSpeed->value() / 100.0f;
    m_pAnimConfig->internalBezier = g_pGlobalState->config.animationBezier->value();

    m_bButtonsDirty      = true;
    m_bTitleColorChanged = true;
    m_pTextTex           = nullptr;

    auto pWin = m_pWindow.lock();
    if (pWin) {
        bool focused = pWin == Desktop::focusState()->window();
        m_bWindowHasFocus = focused;
    }

    invalidateButtonTextures();
    ensureButtonInstances();
    updateButtonStateAnimations();

    g_pDecorationPositioner->repositionDeco(this);
    damageEntire();
}

void CHyprBar::damageEntire() {
    g_pHyprRenderer->damageBox(assignedBoxGlobal());
}

Vector2D CHyprBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

eDecorationLayer CHyprBar::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CHyprBar::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT | (g_pGlobalState->config.barPartOfWindow->value() ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox CHyprBar::assignedBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    CBox box = m_bAssignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW CHyprBar::getOwner() {
    return m_pWindow.lock();
}

void CHyprBar::updateRules() {
    const auto PWINDOW              = m_pWindow.lock();
    auto       prevHidden           = m_hidden;
    auto       prevForcedTitleColor = m_bForcedTitleColor;

    m_bForcedBarColor   = std::nullopt;
    m_bForcedTitleColor = std::nullopt;
    m_hidden            = false;

    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->nobarRuleIdx))
        m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->nobarRuleIdx)->effect);
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->barColorRuleIdx))
        m_bForcedBarColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->barColorRuleIdx)->effect).value_or(0));
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->titleColorRuleIdx))
        m_bForcedTitleColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->titleColorRuleIdx)->effect).value_or(0));

    if (prevHidden != m_hidden)
        g_pDecorationPositioner->repositionDeco(this);
    if (prevForcedTitleColor != m_bForcedTitleColor)
        m_bTitleColorChanged = true;
}

void CHyprBar::damageOnButtonHover() {
    if (!m_bWindowHasFocus) {
        if (m_bMouseOnBar || m_iButtonHoverStateRight != 0 || m_iButtonHoverStateLeft != 0) {
            m_bMouseOnBar = false;
            m_iButtonHoverStateLeft = 0;
            m_iButtonHoverStateRight = 0;
        }
        return;
    }

    const auto COORDS           = cursorRelativeToBar();
    const auto DECOBOX          = assignedBoxGlobal();
    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUF           = Vector2D{(int)DECOBOX.w, HEIGHT};

    bool mouseOnBar = VECINRECT(COORDS, 0, 0, BARBUF.x, BARBUF.y);
    bool changed = (mouseOnBar != m_bMouseOnBar);
    if (changed) {
        m_bMouseOnBar = mouseOnBar;

        updateButtonStateAnimations();
        invalidateButtonTextures();
        damageEntire();
    }
    if (!m_bMouseOnBar) {
        m_iButtonHoverStateLeft = 0;
        m_iButtonHoverStateRight = 0;
        return;
    }

    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto SCALEPADDING     = effectivePaddingScale();

    auto checkSide = [&](const std::vector<size_t>& indices,
                         unsigned int& hoverBits, bool isRight) -> bool {
        if (indices.empty()) return false;

        bool changed = false;

        float offset = BARPADDING;

        for (size_t idx = 0; idx < indices.size(); ++idx) {
            const auto&  button    = g_pGlobalState->buttons[indices[idx]];
            const float  btnScale  = m_vButtonInstances[indices[idx]].scale->value();
            const float  slotSize  = effectiveButtonSlot(button.size, btnScale, idx == 0);

            const float  centerX   = isRight ? (BARBUF.x - offset - slotSize / 2.0f)
                                             : (offset + slotSize / 2.0f);
            const float  centerY   = BARBUF.y / 2.0f;

            const double distSq    = std::pow(COORDS.x - centerX, 2) + std::pow(COORDS.y - centerY, 2);
            const double radiusSq  = std::pow(button.size / 2.0f, 2);

            const bool   hover     = m_bMouseOnBar && (distSq <= radiusSq);

            if (hover != ((hoverBits & (1 << idx)) != 0)) {
                hoverBits ^= (1 << idx);
                changed = true;
            }

            offset += slotSize + BARBUTTONPADDING * SCALEPADDING;
        }
        return changed;
    };

    bool cl = checkSide(g_pGlobalState->buttonsLeft, m_iButtonHoverStateLeft, false);
    bool cr = checkSide(g_pGlobalState->buttonsRight, m_iButtonHoverStateRight, true);

    if (cl || cr) {
        if (m_iPressedIdx >= 0) {
            if (m_bPressedIsRight)
                m_iButtonHoverStateRight &= ~(1 << m_iPressedIdx);
            else
                m_iButtonHoverStateLeft  &= ~(1 << m_iPressedIdx);
        }
        updateButtonStateAnimations();
    }
}
