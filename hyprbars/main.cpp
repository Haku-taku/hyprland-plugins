#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <any>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/config/lua/types/LuaConfigColor.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <hyprland/src/config/values/types/FloatValue.hpp>

#include <hyprutils/string/VarList.hpp>

#include <algorithm>

#include "barDeco.hpp"
#include "globals.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static int parseLuaButton(lua_State* L, bool isLeft);
static void parseLuaButtonState(lua_State* L, SHyprButtonState& state);

static void onNewWindow(PHLWINDOW window) {
    if (!window->m_X11DoesntWantBorders) {
        if (std::ranges::any_of(window->m_windowDecorations, [](const auto& d) { return d->getDisplayName() == "Hyprbar"; }))
            return;

        auto bar = makeUnique<CHyprBar>(window);
        g_pGlobalState->bars.emplace_back(bar);
        bar->m_self = bar;
        HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(bar));
    }
}

static void onPreConfigReload() {
    if (g_pLuaState) {
        auto unref = [&](SHyprButtonAction& a) {
            if (a.callback != LUA_NOREF) luaL_unref(g_pLuaState, LUA_REGISTRYINDEX, a.callback);
        };
        unref(g_pGlobalState->config.onDoubleClick);
        for (auto& button : g_pGlobalState->buttons) {
            unref(button.click);
            unref(button.rightClick);
            unref(button.middleClick);
            unref(button.scrollUp);
            unref(button.scrollDown);
            unref(button.scrollClick);
        }
    }
    g_pGlobalState->buttonsLeft.clear();
    g_pGlobalState->buttonsRight.clear();
    g_pGlobalState->buttons.clear();
}

static void onConfigReloaded() {
    for (auto& b : g_pGlobalState->bars) {
        if (!b)
            continue;

        b->onConfigReloaded();
    }
}

static void onUpdateWindowRules(PHLWINDOW window) {
    const auto BARIT = std::find_if(g_pGlobalState->bars.begin(), g_pGlobalState->bars.end(), [window](const auto& bar) { return bar->getOwner() == window; });

    if (BARIT == g_pGlobalState->bars.end())
        return;

    (*BARIT)->updateRules();
    window->updateWindowDecos();
}

int newLuaButton(lua_State* L) {
    return parseLuaButton(L, false);
}
int newLuaButtonLeft(lua_State* L)   { return parseLuaButton(L, true); }
int newLuaButtonRight(lua_State* L)  { return parseLuaButton(L, false); }

static int luaOnDoubleClick(lua_State* L) {
    g_pLuaState = L;
    auto& action = g_pGlobalState->config.onDoubleClick;

    // Release any previous callback.
    if (action.callback != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, action.callback);
        action.callback   = LUA_NOREF;
        action.dispatcher = "";
    }

    if (lua_isstring(L, 1)) {
        action.dispatcher = lua_tostring(L, 1);
    } else if (!lua_isnil(L, 1)) {
        lua_pushvalue(L, 1);
        action.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int parseLuaButton(lua_State* L, bool isLeft) {
    if (!lua_istable(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "add_button: expected a table { bg_color, fg_color, size, icon, click, ... }");

    // Keep the Lua state for later callback execution.
    g_pLuaState = L;

    SHyprButton button;

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "bg_color");

        Config::Lua::CLuaConfigColor parser(0);
        auto                         err = parser.parse(L);
        if (err.errorCode != Config::Lua::PARSE_ERROR_OK)
            return Config::Lua::Bindings::Internal::configError(L, "add_button: failed to parse bg_color");

        button.bgcol = parser.parsed();
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "fg_color");

        if (!lua_isnil(L, -1)) {
            Config::Lua::CLuaConfigColor parser(0);
            auto                         err = parser.parse(L);
            if (err.errorCode != Config::Lua::PARSE_ERROR_OK)
                return Config::Lua::Bindings::Internal::configError(L, "add_button: failed to parse fg_color");

            button.userfg = true;
            button.fgcol = parser.parsed();
        }
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "size");

        if (!lua_isnumber(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "add_button: size must be a number");

        button.size = lua_tonumber(L, -1);
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "icon");

        if (!lua_isstring(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "add_button: icon must be a string");

        button.icon = lua_tostring(L, -1);
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "inactive_color");

        if (!lua_isnil(L, -1)) {
            Config::Lua::CLuaConfigColor parser(0);
            if (parser.parse(L).errorCode == Config::Lua::PARSE_ERROR_OK)
                button.inactiveBgColor = parser.parsed();
        }
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "inactive_scale");

        if (lua_isnumber(L, -1))
            button.inactiveScale = (float)lua_tonumber(L, -1);
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "inactive_icon_scale");

        if (lua_isnumber(L, -1))
            button.inactiveIconScale = (float)lua_tonumber(L, -1);
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "hover");

        if (lua_istable(L, -1))
            parseLuaButtonState(L, button.stateHover);
    }

    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });

        lua_getfield(L, 1, "press");

        if (lua_istable(L, -1))
            parseLuaButtonState(L, button.statePress);
    }

    auto parseAction = [&](const char* field, SHyprButtonAction& action) {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, field);
        if (lua_isstring(L, -1)) {
            action.dispatcher = lua_tostring(L, -1);
        } else if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, -1);
            action.callback = luaL_ref(L, LUA_REGISTRYINDEX);
        }
    };
    parseAction("click",         button.click);
    parseAction("right_click",   button.rightClick);
    parseAction("middle_click",  button.middleClick);
    parseAction("scroll_up",     button.scrollUp);
    parseAction("scroll_down",   button.scrollDown);
    parseAction("scroll_click",  button.scrollClick);

    size_t idx = g_pGlobalState->buttons.size();
    g_pGlobalState->buttons.push_back(std::move(button));
    if (isLeft)
        g_pGlobalState->buttonsLeft.push_back(idx);
    else
        g_pGlobalState->buttonsRight.push_back(idx);

    for (auto& b : g_pGlobalState->bars) {
        if (!b)
            continue;
        b->m_bButtonsDirty = true;
    }

    return 0;
}

static void parseLuaButtonState(lua_State* L, SHyprButtonState& state) {
    {
        lua_getfield(L, -1, "bg_color");
        if (!lua_isnil(L, -1)) {
            Config::Lua::CLuaConfigColor parser(0);
            if (parser.parse(L).errorCode == Config::Lua::PARSE_ERROR_OK)
                state.bgColor = parser.parsed();
        }
        lua_pop(L, 1);
    }

    {
        lua_getfield(L, -1, "fg_color");
        if (!lua_isnil(L, -1)) {
            Config::Lua::CLuaConfigColor parser(0);
            if (parser.parse(L).errorCode == Config::Lua::PARSE_ERROR_OK)
                state.fgColor = parser.parsed();
        }
        lua_pop(L, 1);
    }

    {
        lua_getfield(L, -1, "icon");
        if (lua_isstring(L, -1)) state.icon = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    {
        lua_getfield(L, -1, "scale");
        if (lua_isnumber(L, -1)) state.scale = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }

    {
        lua_getfield(L, -1, "icon_scale");
        if (lua_isnumber(L, -1)) state.iconScale = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprbars] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hb] Version mismatch");
    }

    g_pGlobalState                    = makeUnique<SGlobalState>();
    g_pGlobalState->nobarRuleIdx      = Desktop::Rule::windowEffects()->registerEffect("hyprbars:no_bar");
    g_pGlobalState->barColorRuleIdx   = Desktop::Rule::windowEffects()->registerEffect("hyprbars:bar_color");
    g_pGlobalState->titleColorRuleIdx = Desktop::Rule::windowEffects()->registerEffect("hyprbars:title_color");

    static auto P  = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); });
    static auto P3 = Event::bus()->m_events.window.updateRules.listen([&](PHLWINDOW w) { onUpdateWindowRules(w); });

    g_pGlobalState->config.barColor            = makeShared<Config::Values::CColorValue>("plugin:hyprbars:bar_color", "Change the bar color", 0x88333333);
    g_pGlobalState->config.textColor           = makeShared<Config::Values::CColorValue>("plugin:hyprbars:col.text", "Change the text color", 0xffffffff);
    g_pGlobalState->config.inactiveButtonColor = makeShared<Config::Values::CColorValue>(
        "plugin:hyprbars:inactive_button_color", "Change the inactive button's color. 0x00000000 means unset", 0x00000000);
    g_pGlobalState->config.barHeight       = makeShared<Config::Values::CIntValue>("plugin:hyprbars:bar_height", "Change the bar's height", 15);
    g_pGlobalState->config.barTextSize     = makeShared<Config::Values::CIntValue>("plugin:hyprbars:bar_text_size", "Change the bar's text size", 10);
    g_pGlobalState->config.barTextWeight   = makeShared<Config::Values::CFontWeightValue>("plugin:hyprbars:bar_text_weight", "Bar's title text weight (e.g. \"bold\" or an integer 100-1000)", 400);
    g_pGlobalState->config.barTitleEnabled = makeShared<Config::Values::CBoolValue>("plugin:hyprbars:bar_title_enabled", "Whether to enable titles in the bar", true);
    g_pGlobalState->config.barBlur         = makeShared<Config::Values::CBoolValue>("plugin:hyprbars:bar_blur", "Whether to enable blur of the bar", false);
    g_pGlobalState->config.barTextFont     = makeShared<Config::Values::CStringValue>("plugin:hyprbars:bar_text_font", "Bar's text font", "Sans");
    g_pGlobalState->config.barTextAlign    = makeShared<Config::Values::CStringValue>("plugin:hyprbars:bar_text_align", "Bar's text alignment", "center");
    g_pGlobalState->config.barPartOfWindow =
        makeShared<Config::Values::CBoolValue>("plugin:hyprbars:bar_part_of_window", "Whether the bar is a part of the window (reserves space)", true);
    g_pGlobalState->config.barPrecedenceOverBorder =
        makeShared<Config::Values::CBoolValue>("plugin:hyprbars:bar_precedence_over_border", "Whether the bar is before, or after the border", false);
    g_pGlobalState->config.barPadding          = makeShared<Config::Values::CIntValue>("plugin:hyprbars:bar_padding", "Padding of the bar", 7);
    g_pGlobalState->config.barButtonPadding    = makeShared<Config::Values::CIntValue>("plugin:hyprbars:bar_button_padding", "Padding of the bar buttons", 5);
    g_pGlobalState->config.inactivePadScale    = makeShared<Config::Values::CFloatValue>("plugin:hyprbars:inactive_padding_scale", "Whether the padding of the buttons should be scaled along with them", 1.0f);
    g_pGlobalState->config.enabled             = makeShared<Config::Values::CBoolValue>("plugin:hyprbars:enabled", "Whether bars are enabled", true);
    g_pGlobalState->config.iconOnHover         = makeShared<Config::Values::CIntValue>("plugin:hyprbars:icon_on_hover", "Icon visibility mode: 0=always show when focused, 1=show when mouse is on the bar, 2=show only when hovering a specific button", 0);
    g_pGlobalState->config.animationSpeed      = makeShared<Config::Values::CIntValue>("plugin:hyprbars:animation_speed", "Animation speed in ms for button transitions", 150);
    g_pGlobalState->config.animationBezier     = makeShared<Config::Values::CStringValue>("plugin:hyprbars:animation_bezier", "Animation bezier curve name (e.g. \"default\", \"easeOut\", or a custom bezier name)", "");
    g_pGlobalState->config.inactiveScale       = makeShared<Config::Values::CFloatValue>("plugin:hyprbars:inactive_scale", "Default scale for buttons in inactive state", 1.0f);
    g_pGlobalState->config.fixButtonCenter     = makeShared<Config::Values::CBoolValue>("plugin:hyprbars:fix_button_center", "When true buttons keep centers fixed when scaled; when false positions shift and inactivePadScale applies", true);
    g_pGlobalState->config.iconScale           = makeShared<Config::Values::CFloatValue>("plugin:hyprbars:icon_scale", "Icon/text glyph size as a fraction of the button circle diameter", 0.62f);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barColor);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.textColor);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.inactiveButtonColor);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barHeight);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barTextSize);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barTextWeight);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barTitleEnabled);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barBlur);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barTextFont);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barTextAlign);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barPartOfWindow);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barPrecedenceOverBorder);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barPadding);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.barButtonPadding);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.inactivePadScale);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.enabled);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.iconOnHover);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.animationSpeed);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.animationBezier);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.inactiveScale);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.fixButtonCenter);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalState->config.iconScale);

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbars", "add_button", ::newLuaButton);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbars", "add_button_left", ::newLuaButtonLeft);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbars", "add_button_right", ::newLuaButtonRight);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprbars", "on_double_click", ::luaOnDoubleClick);

    static auto P4 = Event::bus()->m_events.config.preReload.listen([&] { onPreConfigReload(); });
    static auto P5 = Event::bus()->m_events.config.reloaded.listen([&] { onConfigReloaded(); });

    // add deco to existing windows
    for (auto& w : g_pCompositor->m_windows) {
        if (w->isHidden() || !w->m_isMapped)
            continue;

        onNewWindow(w);
    }

    HyprlandAPI::reloadConfig();

    return {"hyprbars", "A plugin to add title bars to windows.", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto& m : State::monitorState()->monitors())
        m->m_scheduledRecalc = true;

    g_pHyprRenderer->m_renderPass.removeAllOfType("CBarPassElement");

    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->barColorRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->titleColorRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_pGlobalState->nobarRuleIdx);
}
