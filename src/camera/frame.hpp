// The camera core's per-frame contract with a view processor (docs/design.md, "Core and processors"). The core
// owns the hook, the player, cuts, the crossfade, layers and the API; a processor (Smoothwalker's follow,
// smoothwalker/follow/processor.hpp) gets the game's view of the player's camera and hands back where to put it.
// Numbers only, no Unreal or UE4SS types: a processor runs on the task-graph workers that call GetCameraView and
// never calls into UObjects. Standard layout with a size at the head, so the C table (camera/api.hpp) carries it.
#pragma once

#include "../common/math.hpp"

#include <cstdint>

namespace dw::camera
{
    // Filled by the core for one player-camera update. Whatever else a processor reads (its own settings, mode
    // writes, the aiming / combat / traversal flags) is its own.
    struct FrameIn
    {
        uint32_t size;       // sizeof(FrameIn) as the core was built
        bool enabled;        // the processor's own switch as the core sampled it for this update (Processor::state)
        bool restart;        // a hard cut or the first frame after one: start again from the capsule, show the game's view
        double dt;           // s, world delta, clamped to 0..0.1
        dw::Vec3 pivot;      // capsule centre, world space
        double half_height;  // capsule half height; NAN when unknown or implausible
        dw::Vec3 camera;     // the game's camera location this frame
        dw::Quat rotation; // the game's camera rotation this frame
    };

    // Filled by the processor. The core value-initializes it before the call, so `size` is set.
    struct FrameOut
    {
        uint32_t size = sizeof(FrameOut);
        dw::Vec3 location;    // where the camera goes; the game's camera unless the processor moved it
        dw::Quat rotation;    // its rotation; the game's unless `rotated`
        bool rotated = false; // rotation differs from the game's and is written to the view

        // Bumped whenever something the processor shows changes without a cut (a setting, a camera-mode write): the
        // core crossfades on it. Reported with the frame it was read for, so a change never shows for a frame before
        // its crossfade starts.
        uint64_t generation = 0;
        double transition = 0.0; // s the core's crossfade runs after any change (position_transition); 0: none

        // For the core's crossfade: keep the faded part of the lag in front of a wall the game pulled in for.
        bool wall_clamp = false;
        double nominal_distance = 0.0; // the processor's recent camera distance from the pivot

        // The debug overlay's feed, valid only while `feed`; the core keeps it and hands it back (Api::read_debug).
        bool feed = false;
        double keep_follow = 0.0, keep_turn = 0.0, rate_h = 0.0;
        int32_t influence = 0; // dw::Influence
    };
} // namespace dw::camera
