#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/FontWeightValue.hpp>

#include <optional>

inline HANDLE PHANDLE = nullptr;

struct SHyprButtonState {
    std::optional<CHyprColor>  fgColor;
    std::optional<CHyprColor>  bgColor;
    std::optional<float>       scale;
    std::optional<std::string> icon;
    std::optional<float>       iconScale;

    bool hasAnyOverride() const {
        return fgColor.has_value() || bgColor.has_value() || icon.has_value() || scale.has_value() || iconScale.has_value();
    }
};

struct SHyprButtonAction {
    int         callback   = LUA_NOREF;
    std::string dispatcher;
};

struct SHyprButton {
    SHyprButtonAction click;
    SHyprButtonAction rightClick;
    SHyprButtonAction middleClick;
    SHyprButtonAction scrollUp;
    SHyprButtonAction scrollDown;
    SHyprButtonAction scrollClick;

    bool       userfg  = false;
    CHyprColor fgcol   = CHyprColor(0, 0, 0, 0);
    CHyprColor bgcol   = CHyprColor(0, 0, 0, 0);
    float      size    = 10;
    std::string icon    = "";


    std::optional<CHyprColor> inactiveBgColor;
    std::optional<float>      inactiveScale;
    std::optional<float>      inactiveIconScale;

    float iconScaleDefault = 1.0f;

    SHyprButtonState          stateDefault;
    SHyprButtonState          stateHover;
    SHyprButtonState          statePress;

    bool hasPerStateConfig() const {
        return stateDefault.hasAnyOverride() || stateHover.hasAnyOverride() || statePress.hasAnyOverride();
    }

    const SHyprButtonState* getStateOverride(int state) const {
        switch (state) {
            case 1: return stateDefault.hasAnyOverride() ? &stateDefault : nullptr;
            case 2: return stateHover.hasAnyOverride()   ? &stateHover   : nullptr;
            case 3: return statePress.hasAnyOverride()   ? &statePress   : nullptr;
            default: return nullptr;
        }
    }

    CHyprColor getFgColor(int state) const {
        if (auto* s = getStateOverride(state); s && s->fgColor.has_value())
            return *s->fgColor;
        if (state == 3 && stateHover.fgColor.has_value())
            return *stateHover.fgColor;
        return fgcol;
    }

    CHyprColor getBgColor(int state) const {
        if (auto* s = getStateOverride(state); s && s->bgColor.has_value())
            return *s->bgColor;
        if (state == 3 && stateHover.bgColor.has_value())
            return *stateHover.bgColor;
        return bgcol;
    }

    std::string getIcon(int state) const {
        if (auto* s = getStateOverride(state); s && s->icon.has_value())
            return *s->icon;
        if (state == 3 && stateHover.icon.has_value())
            return *stateHover.icon;
        return icon;
    }

    float getScale(int state) const {
        if (auto* s = getStateOverride(state); s && s->scale.has_value())
            return *s->scale;
        if (state == 3 && stateHover.scale.has_value())
            return *stateHover.scale;
        return 1.0f;
    }

    float getIconScale(int state) const {
        if (auto* s = getStateOverride(state); s && s->iconScale.has_value())
            return *s->iconScale;
        if (state == 3 && stateHover.iconScale.has_value())
            return *stateHover.iconScale;
        return iconScaleDefault;
    }

    CHyprColor getAutoFgColor(int state) const {
        CHyprColor fg = getFgColor(state);
        if (fg.r == 0 && fg.g == 0 && fg.b == 0 && fg.a == 0) {
            CHyprColor bg = getBgColor(state);
            return (bg.r + bg.g + bg.b < 1.5f) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);
        }
        return fg;
    }
};

struct SHyprButtonInstance {
    size_t buttonIdx = 0;

    SP<Render::ITexture> texDefault;
    SP<Render::ITexture> texHover;
    SP<Render::ITexture> texPress;

    int   currentState       = 0;
    int   prevState          = 0;
    bool  needsTextureUpdate = true;
    float cachedButtonSize   = 0.0f;

    PHLANIMVAR<float>         crossfadeProgress;
    PHLANIMVAR<CHyprColor>    bgColor;
    PHLANIMVAR<float>         scale;
    PHLANIMVAR<float>         iconAlpha;
    PHLANIMVAR<CHyprColor>    iconColor;
    PHLANIMVAR<float>         iconScale;
};

class CHyprBar;

struct SGlobalState {
    std::vector<SHyprButton>  buttons;

    std::vector<size_t>       buttonsLeft;
    std::vector<size_t>       buttonsRight;

    std::vector<WP<CHyprBar>> bars;
    uint32_t                  nobarRuleIdx      = 0;
    uint32_t                  barColorRuleIdx   = 0;
    uint32_t                  titleColorRuleIdx = 0;

    struct {
        SP<Config::Values::CIntValue>        animationSpeed;
        SP<Config::Values::CStringValue>     animationBezier;
        SP<Config::Values::CColorValue>      barColor, textColor, inactiveButtonColor;
        SP<Config::Values::CIntValue>        barHeight;
        SP<Config::Values::CIntValue>        barTextSize;
        SP<Config::Values::CFontWeightValue> barTextWeight;
        SP<Config::Values::CIntValue>        barPadding;
        SP<Config::Values::CIntValue>        barButtonPadding;
        SP<Config::Values::CBoolValue>       barBlur, barTitleEnabled, barPartOfWindow, barPrecedenceOverBorder, enabled, fixButtonCenter;
        SP<Config::Values::CStringValue>     barTextFont, barTextAlign, onDoubleClick;
        SP<Config::Values::CFloatValue>      iconScale, inactivePadScale, inactiveScale;
        SP<Config::Values::CIntValue>        iconOnHover;
    } config;
};

inline UP<SGlobalState> g_pGlobalState;
inline lua_State*       g_pLuaState = nullptr;

void executeButtonAction(const SHyprButtonAction& action);
