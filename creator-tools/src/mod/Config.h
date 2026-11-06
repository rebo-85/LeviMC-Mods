#pragma once

#include <array>
#include <string>

namespace creator_tools {

struct Config {
    int version = 1;

    struct Hotkeys {
        std::string gamemode = "G";
        std::string reload   = "R";
    } hotkeys;

    struct EntityDebug {
        bool inspectCtrl  = true;
        bool inspectShift = true;
        bool inspectAlt   = false;

        bool  hideNameTag  = true;
        float labelScale   = 0.3f;
        int   maxBehaviors = 24;

        bool                showPath         = true;
        float               pathHeightOffset = 0.1f;
        float               pathTargetRadius = 0.25f;
        float               pathDestRadius   = 0.40f;
        std::array<float, 4> pathLineColor   = {1.0f, 0.65f, 0.1f, 1.0f};
        std::array<float, 4> pathTargetColor = {1.0f, 1.0f, 0.2f, 1.0f};
        std::array<float, 4> pathDestColor   = {1.0f, 0.2f, 0.2f, 1.0f};
    } entityDebug;
};

} // namespace creator_tools
