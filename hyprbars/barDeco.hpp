#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include "globals.hpp"

#define private public
#include <hyprland/src/managers/input/InputManager.hpp>
#undef private

namespace Event {
    struct SCallbackInfo;
}

class CHyprBar : public IHyprWindowDecoration {
  public:
    CHyprBar(PHLWINDOW);
    virtual ~CHyprBar();

    virtual SDecorationPositioningInfo getPositioningInfo();

    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);

    virtual void                       draw(PHLMONITOR, float const& a);

    virtual eDecorationType            getDecorationType();

    virtual void                       updateWindow(PHLWINDOW);

    virtual void                       damageEntire();

    virtual eDecorationLayer           getDecorationLayer();

    virtual uint64_t                   getDecorationFlags();

    bool                               m_bButtonsDirty = true;

    virtual std::string                getDisplayName();

    PHLWINDOW                          getOwner();

    void                               updateRules();
    void                               onConfigReloaded();

    WP<CHyprBar>                       m_self;

  private:
    SBoxExtents                m_seExtents;

    PHLWINDOWREF               m_pWindow;

    CBox                       m_bAssignedBox;

    SP<Render::ITexture>       m_pTextTex;

    bool                       m_bWindowSizeChanged = false;
    bool                       m_hidden             = false;
    bool                       m_bTitleColorChanged = false;
    bool                       m_bLastEnabledState  = false;
    bool                       m_bWindowHasFocus    = false;
    std::optional<CHyprColor>  m_bForcedBarColor;
    std::optional<CHyprColor>  m_bForcedTitleColor;

    Time::steady_tp            m_lastMouseDown = Time::steadyNow();

    PHLANIMVAR<CHyprColor>     m_cRealBarColor;
    int                        m_iPressedIdx     = -1;
    uint32_t                   m_iPressedButton  = 0;
    bool                       m_bPressedIsRight = false;
    bool                       m_bRendering      = false;
    bool                       m_bMouseOnBar     = false;

    std::vector<SHyprButtonInstance> m_vButtonInstances;

    Vector2D                   cursorRelativeToBar();

    void                       renderPass(PHLMONITOR, float const& a);
    void                       renderBarTitle(const Vector2D& bufferSize, const float scale);
    void renderBarButtons(CBox* barBox, const float scale, const float a);
    void damageOnButtonHover();

    void                       ensureButtonInstances();
    void                       updateButtonStateAnimations();
    SP<Render::ITexture>       getOrCreateIconTexture(SHyprButtonInstance& inst, int state, float buttonSize, CHyprColor col);
    SP<Render::ITexture>       loadSvgIcon(const std::string& path, float pxSize);
    void                       invalidateButtonTextures();

    bool inputIsValid();
    void onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e);
    void onTouchDown(Event::SCallbackInfo& info, ITouch::SDownEvent e);
    void onTouchUp(Event::SCallbackInfo& info, ITouch::SUpEvent e);
    void onMouseMove(Vector2D coords);
    void onTouchMove(Event::SCallbackInfo& info, ITouch::SMotionEvent e);
    void onMouseAxis(Event::SCallbackInfo& info, IPointer::SAxisEvent e);

    void handleDownEvent(Event::SCallbackInfo& info, std::optional<ITouch::SDownEvent> touchEvent);
    void handleUpEvent(Event::SCallbackInfo& info);
    void handleMovement();
    bool doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS);

    CBox assignedBoxGlobal();

    CHyprSignalListener m_pMouseButtonCallback;
    CHyprSignalListener m_pTouchDownCallback;
    CHyprSignalListener m_pTouchUpCallback;

    CHyprSignalListener m_pTouchMoveCallback;
    CHyprSignalListener m_pMouseMoveCallback;
    CHyprSignalListener m_pMouseAxisCallback;

    std::string         m_szLastTitle;

    bool                m_bDraggingThis  = false;
    bool                m_bTouchEv       = false;
    bool                m_bDragPending   = false;
    bool                m_bCancelledDown = false;
    int                 m_touchId        = 0;

    // store hover state for buttons as a bitfield
    unsigned int m_iButtonHoverStateLeft  = 0;
    unsigned int m_iButtonHoverStateRight = 0;
    // store press state for buttons as a bitfield
    unsigned int m_iButtonPressStateLeft  = 0;
    unsigned int m_iButtonPressStateRight = 0;

    SP<Hyprutils::Animation::SAnimationPropertyConfig> m_pAnimConfig;
    PHLANIMVAR<float>    m_fPaddingScale;

    // for dynamic updates
    int    m_iLastHeight = 0;
    int    m_fLastScale  = 0;

    size_t getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D& bufferSize, const float scale);

    float  effectivePaddingScale() const;
    float  effectiveButtonSlot(float buttonSize, float btnScale, bool isFirst = false) const;

    friend class CBarPassElement;
};
