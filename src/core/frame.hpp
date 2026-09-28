// The camera core's per-frame contract with a view processor (docs/design.md, "Core and processors"). The core
// owns the hook, the player, cuts, the crossfade, layers and the API; a processor (the follow, follow/follow.hpp)
// gets the game's view of the player's camera and hands back where to put it. Numbers only, no Unreal or UE4SS
// types: a processor runs on the task-graph workers that call GetCameraView and never calls into UObjects.
// Standard layout on purpose, so a later C ABI between two DLLs can carry it unchanged.
#pragma once

#include "../smoothing.hpp"

#include <cstdint>

namespace dwcam
{
    struct FrameIn
    {
        bool enabled;       // the core's live switch (O, the ini): off, a processor only keeps its bookkeeping
        bool restart;       // a hard cut or the first frame after one: start again from the capsule, show the game's view
        bool mode_write;    // a camera-mode write landed since the last frame: its distance glides in over `transition`
        double transition;  // s, position_transition
        double dt;          // s, world delta, clamped to 0..0.1
        dwsc::Vec3 pivot;   // capsule centre, world space
        double half_height; // capsule half height; NAN when unknown or implausible
        dwsc::Vec3 camera;  // the game's camera location this frame
        dwsc::Quat rotation; // the game's camera rotation this frame
        bool aiming, combat, traversal; // a camera mode of that group is blending in or active
    };

    struct FrameOut
    {
        dwsc::Vec3 location;  // where the camera goes; the game's camera unless the processor moved it
        dwsc::Quat rotation;  // its rotation; the game's unless `rotated`
        bool rotated = false; // rotation differs from the game's and is written to the view

        // Bumped whenever a setting the processor reads changes: the core crossfades on it. Reported with the
        // frame it was read for, so a change never shows for a frame before its crossfade starts.
        uint64_t generation = 0;

        // For the core's crossfade: keep the faded part of the lag in front of a wall the game pulled in for.
        bool wall_clamp = false;
        double nominal_distance = 0.0; // the processor's recent camera distance from the pivot

        // The debug overlay's feed, valid only while `feed`.
        bool feed = false;
        double keep_follow = 0.0, keep_turn = 0.0, rate_h = 0.0;
        int influence = 0; // dwsc::Influence

        // log_stats, valid only while `stats`.
        bool stats = false;
        bool clamped = false; // the wall clamp moved the result
        double shown_lag = 0.0; // cm between the pivot and the shown pivot
    };

    // A processor: `frame` runs once per player-camera update that reaches the pipeline, on the hook thread, in
    // sequence, whether the core is on or off (off: bookkeeping only). One processor for now (the follow); a list
    // comes with the split into two DLLs.
    struct Processor
    {
        void* user;
        void (*frame)(void* user, const FrameIn& in, FrameOut& out);
    };
} // namespace dwcam
