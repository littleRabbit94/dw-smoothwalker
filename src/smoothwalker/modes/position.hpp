// The camera modes' value types: the tuning groups and what the debug overlay lists of a live mode. Pure, no Unreal
// headers, so the unit tests and the panel formatter (ui/panel.hpp) compile it; the tuner that writes them into the
// game is modes/mode_tuner.hpp.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include <cstdint>
#include <string>

namespace dw::smoothwalker::modes
{
    enum Group : int
    {
        Exploration,
        Sprint,
        Combat,
        Focus,
        Aiming,
        Traversal,
        GroupCount
    };

    inline auto group_name(Group group) -> const wchar_t*
    {
        static constexpr const wchar_t* names[GroupCount]{L"Exploration", L"Sprint", L"Combat", L"Focus", L"Aiming", L"Traversal"};
        return group >= 0 && group < GroupCount ? names[group] : L"?";
    }

    // One live mode on the player's camera, for the debug overlay.
    struct ModeListing
    {
        std::wstring name; // BP_CameraMode_<name>_C, or the class name without _C
        uint8_t state;     // ECameraModeState: 0 blending in, 1 active, 2 blending out
        int group;         // Group; -1 for a mode this mod does not track
        bool tuned;        // tracked, and the last apply wrote the camera position into it
    };
} // namespace dw::smoothwalker::modes
