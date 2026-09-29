// The camera core's per-frame contract with a view processor (docs/design.md, "Core and processors"). The core
// owns the hook, the player, cuts, the crossfade, layers and the API; a processor (Smoothwalker's follow,
// smoothwalker/follow/processor.hpp) gets the game's view of the player's camera and hands back where to put it.
// Numbers only, no Unreal or UE4SS types: a processor runs on the task-graph workers that call GetCameraView and
// never calls into UObjects.
#pragma once

#include "../common/math.hpp"

#include <cstdint>

namespace dw::camera
{
    // Filled by the core for one player-camera update. Whatever else a processor reads (its own settings, mode
    // writes, the aiming / combat / traversal flags) is its own.
    struct FrameIn
    {
        bool enabled;        // the processor's own switch as the core sampled it for this update (Processor::state)
        bool restart;        // a hard cut or the first frame after one: start again from the capsule, show the game's view
        double dt;           // s, world delta, clamped to 0..0.1
        dw::Vec3 pivot;      // capsule centre, world space
        double half_height;  // capsule half height; NAN when unknown or implausible
        dw::Vec3 camera;     // the game's camera location this frame
        dw::Quat rotation; // the game's camera rotation this frame
    };

    // Filled by the processor. The core value-initializes it before the call.
    struct FrameOut
    {
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

        // The debug overlay's feed, valid only while `feed`; the core keeps it and hands it back (CameraCore::read_debug).
        bool feed = false;
        double keep_follow = 0.0, keep_turn = 0.0, rate_h = 0.0;
        int32_t influence = 0; // follow::Influence
    };

    // Why the follow last restarted from the capsule (a hard cut), for the debug overlay. Whoever sets the hook's
    // reset flag names the reason first; the hook works out the rest (a gap, a teleport, the toggle) itself.
    enum class Snap : int
    {
        None,
        Startup,
        Teleport,   // pivot moved more than reset_distance in one update
        Gap,        // no update for more than reset_gap (pause, load, cutscene, free camera)
        World,      // level change
        Player,     // player controller found, changed or gone
        Pawn,       // new or lost pawn
        Toggle,     // switched back on
        ApiCut,     // release("cut"), or a claim dropped by an uninstall or a Lua restart
        ClaimEnded, // a claim's lease ran out, seen by the hook before the game thread dropped it
        ViewLost,   // the pivot or the view could not be read, or a result was not finite
    };
} // namespace dw::camera
