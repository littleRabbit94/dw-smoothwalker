// The camera position as values: the tuning groups, the tuned mode classes, what the settings make of them
// (position_of) and what a publish writes into the game (written), and what the debug overlay lists of a live mode.
// Pure, no Unreal headers, so the unit tests and the panel formatter (ui/panel.hpp) compile it; the tuner that writes
// them into the game is modes/mode_tuner.hpp.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "../settings/settings.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dw::smoothwalker::modes
{
    using settings::Settings;

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

    struct ModeClassSpec
    {
        Group group;
        const wchar_t* name;         // BP_CameraMode_<name>
        const wchar_t* folder = L""; // under Modes/, with its slash
        // Takes the pitch_min / pitch_max settings. The modes shipped at -60 / 40 (measured 2026-09-24 over all
        // 23 CDOs); the rest ship -89 / 89 (AimingOnLadder -40 / 89) and keep their own. A name list rather
        // than a test on the captured values, so a mod that writes limits before the first capture cannot
        // change which modes the setting applies to.
        bool player_pitch = false;
    };

    // Finisher and shadowstep attack cameras are scripted shots and stay as shipped.
    inline const std::vector<ModeClassSpec>& mode_classes()
    {
        constexpr bool pitch = true;
        static const std::vector<ModeClassSpec> specs{
                {Exploration, L"Base", L"", pitch}, {Exploration, L"Base_LongRange", L"", pitch},
                {Exploration, L"Base_CloseRange", L"", pitch}, {Exploration, L"Base_CloseRange_Mantle2m", L"", pitch},
                {Exploration, L"GapSqueeze", L"", pitch},
                {Sprint, L"Sprint", L"", pitch}, {Sprint, L"Sprint_VampiricFastTraversal", L"", pitch},
                {Combat, L"CombatNear"}, {Combat, L"CombatFromArm"}, {Combat, L"CombatFromArm_LongRange"},
                {Combat, L"CombatFromArm_VeryLongRange"}, {Combat, L"CombatFistFightMode"}, {Focus, L"FocusMode", L"", pitch},
                {Combat, L"CombatSprinting"},
                {Aiming, L"Aiming"}, {Aiming, L"AimingOnLadder"}, {Aiming, L"AimingClawRide"}, {Aiming, L"AimingClawRideLedge"},
                {Aiming, L"AntiGravAiming"},
                {Traversal, L"AntiGrav"}, {Traversal, L"ClawRide"}, {Traversal, L"ClawRideLedge"},
                {Traversal, L"Shadowstep_2_Base", L"Shadowstep/", pitch},
        };
        return specs;
    }

    struct GroupTuning
    {
        double distance = 100; // percent
        double height = 0;     // cm
        double shoulder = 0;   // cm outward on the side the mode already sits
        double fov = 0;        // degrees added
    };

    struct PositionTuning
    {
        bool active = true;   // camera_tuning and enabled
        bool own_lag = true;  // enabled: the game's camera lag is off, so the follow is the only lag
        GroupTuning groups[GroupCount];
        bool shoulder_swap = false;
        double pitch_min = -60;
        double pitch_max = 40;
        double transition = 0.5; // s
    };

    inline auto operator==(const GroupTuning& a, const GroupTuning& b) -> bool
    {
        return a.distance == b.distance && a.height == b.height && a.shoulder == b.shoulder && a.fov == b.fov;
    }

    inline auto operator==(const PositionTuning& a, const PositionTuning& b) -> bool
    {
        for (int i = 0; i < GroupCount; ++i)
        {
            if (!(a.groups[i] == b.groups[i])) return false;
        }
        return a.active == b.active && a.own_lag == b.own_lag && a.shoulder_swap == b.shoulder_swap &&
               a.pitch_min == b.pitch_min && a.pitch_max == b.pitch_max && a.transition == b.transition;
    }

    inline auto position_of(const Settings& s) -> PositionTuning
    {
        PositionTuning p;
        p.active = s.camera_tuning && s.enabled;
        p.own_lag = s.enabled;
        p.groups[Exploration] = {s.exploration_distance, s.exploration_height, s.exploration_shoulder, s.exploration_fov};
        p.groups[Sprint] = {s.sprint_distance, s.sprint_height, s.sprint_shoulder, s.sprint_fov};
        p.groups[Combat] = {s.combat_distance, s.combat_height, s.combat_shoulder, s.combat_fov};
        p.groups[Focus] = {s.focus_distance, s.focus_height, s.focus_shoulder, s.focus_fov};
        p.groups[Aiming] = {s.aiming_distance, s.aiming_height, s.aiming_shoulder, s.aiming_fov};
        p.groups[Traversal] = {s.traversal_distance, s.traversal_height, 0.0, s.traversal_fov};
        p.shoulder_swap = s.shoulder_swap;
        p.pitch_min = s.pitch_min;
        p.pitch_max = s.pitch_max;
        p.transition = s.position_transition;
        return p;
    }

    // What a publish writes into the game of a position: with camera_tuning off only the lag switch is written, so a
    // changed position number applies nothing. Two positions with equal written() are the same to the game; the values
    // are kept, so switching camera_tuning on applies the latest.
    inline auto written(PositionTuning p) -> PositionTuning
    {
        if (p.active) return p;
        PositionTuning lag_only;
        lag_only.active = false;
        lag_only.own_lag = p.own_lag;
        return lag_only;
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
