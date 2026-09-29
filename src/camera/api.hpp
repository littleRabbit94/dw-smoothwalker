// The camera core's C interface (docs/design.md, "Core and processors"): the only way Smoothwalker's side reaches
// the core. A table of plain function pointers with its size and version at its head, handed out by
// dwcc_get_api(). Across it: pointers, fixed-width numbers and the standard-layout structs below and in
// camera/frame.hpp; no C++ references, no std types, no exceptions. The table is internal to the one DLL:
// dwcc_get_api() is an ordinary extern "C" function, not a DLL export.
#pragma once

#include "frame.hpp"

#include <cstdint>

namespace dwcam
{
    // 1: the first table. Entries are only ever appended; a caller checks `size` before using a later one.
    constexpr uint32_t API_VERSION = 1;

    // A view processor. The core runs at most one (register_processor answers 0 while one is registered) and calls
    // nothing of it while none is: the hook then leaves the game's view alone unless a layer is set.
    struct Processor
    {
        uint32_t size; // sizeof(Processor)
        void* user;    // handed back to both callbacks
        // Hook thread (a task-graph worker, never the game thread), once per player-camera update that reaches the
        // pipeline, in sequence, whether the processor is on or off (off: bookkeeping only). Numbers only: no
        // UObject, no UE4SS, no lock that a game-thread call can hold for long, no allocation.
        void (*frame)(void* user, const FrameIn* in, FrameOut* out);
        // Any thread (the hook once per player-camera update, before anything else; Lua's enabled() anywhere),
        // lock-free: 1 while the processor is on. Writes its toggle generation, which the processor bumps on every
        // switch before the switch itself, to *toggle_generation. The hook's "off and settled" path is decided on
        // these two alone, so an off processor with nothing left to fade costs one call.
        int32_t (*state)(void* user, uint64_t* toggle_generation);
    };

    // Game-thread notifications for the processor's owner, called from the core's own UE4SS callbacks at fixed
    // points, so its game-thread work keeps one order against player discovery. Any entry may be null.
    struct Listener
    {
        uint32_t size; // sizeof(Listener)
        void* user;
        // Game thread: a level change (LoadMap, or the engine tick seeing a new world). The player is already
        // forgotten; every UObject pointer taken in the old world goes. May come twice for one load.
        void (*world_changed)(void* user);
        // Game thread: a new player camera was just published (player_camera()).
        void (*camera_changed)(void* user);
        // Game thread, every engine tick: after the player controller is checked and discovered, before the pawn and
        // its camera are.
        void (*tick)(void* user);
    };

    // set_diagnostics flags.
    constexpr uint32_t DIAG_VERBOSE = 1u;     // log_verbose: discovery, offsets, hook install
    constexpr uint32_t DIAG_HOOK_TIMING = 2u; // log_stats: time the hook (take_hook_timing)

    // The debug overlay's feed as the hook left it. Relaxed reads: one call may pair values from two updates.
    struct Debug
    {
        uint32_t size;                    // set by the caller to sizeof(Debug); nothing is written if smaller
        double keep_follow, keep_turn;    // the processor's feed (FrameOut) as last reported; NAN: not following
        double rate_h;                    // likewise
        int32_t influence;                // likewise, dwsc::Influence
        double lag_h, lag_v;              // cm of lag on screen, horizontal and vertical; NAN: not following
        int32_t snap;                     // dwsc::Snap of the last restart from the capsule
        double snap_age;                  // s since it; NAN: none yet
        int32_t glide;                    // 1: the running crossfade was started by a release("glide")
    };

    struct Api
    {
        uint32_t size;    // sizeof(Api) as the core was built
        uint32_t version; // API_VERSION as the core was built

        // Game thread or the mod's start thread, never the hook: 1 registered, 0 when one is already registered or
        // p is null, too small or missing a callback. The core keeps the pointer: p must stay valid until unregistered.
        int32_t (*register_processor)(const Processor* p);
        // Game thread or the mod's start thread, never inside a processor call: returns once no call into p is in
        // flight (up to 5 s). 1: drained, or p was not the one registered (nothing could be calling it). 0: timed out,
        // a call may still be running inside p, so the caller must not free anything the callbacks reach.
        int32_t (*unregister_processor)(const Processor* p);
        // Game thread or the mod's start thread: 1 set, 0 when one is set already or l is null or too small. Kept
        // like a processor.
        int32_t (*set_listener)(const Listener* l);
        // Game thread or the mod's start thread, never inside a listener call: returns once no call into l is in
        // flight (up to 5 s). Same result as unregister_processor: 1 drained or not the one set, 0 timed out.
        int32_t (*clear_listener)(const Listener* l);

        // Any thread but the hook (takes the core's settings lock exclusively): the cut thresholds, cm moved in one
        // update that counts as a teleport and s without an update that counts as a gap. A change crossfades like a
        // processor's generation; non-finite values are ignored. The core starts at 500 cm and 0.25 s.
        void (*set_cut_thresholds)(double reset_distance, double reset_gap);
        // Any thread: DIAG_* flags, relaxed.
        void (*set_diagnostics)(uint32_t flags);

        // Any thread: the player's camera component, or null. Compare it anywhere; dereference it on the game thread only.
        void* (*player_camera)();
        // Game thread: the player controller, checked live on this engine tick, or null.
        void* (*player_controller)();
        // Any thread: 1 while a player controller is held.
        int32_t (*player_known)();
        // Any thread: player-camera updates since the core started.
        uint64_t (*view_updates)();
        // Any thread: world seconds summed over those updates (their DeltaTime).
        double (*view_seconds)();
        // Any thread: 1 if the player's camera updated within the last 0.25 s (not paused, loading, in a cutscene or
        // a free camera).
        int32_t (*camera_live)();
        // Any thread: the game thread's id, 0 before the first engine tick.
        uint32_t (*game_thread_id)();

        // Any thread: hook calls timed since the last take and their mean cost in microseconds (0 with no calls),
        // then both counters start again. Timing runs only under DIAG_HOOK_TIMING.
        void (*take_hook_timing)(uint64_t* calls, double* microseconds_per_call);
        // Any thread: the debug overlay's feed into *out (see Debug).
        void (*read_debug)(Debug* out);
        // Any thread (takes the Lua API's mutex briefly): the camera owner's mod name, NUL-terminated and cut to
        // capacity, and the s left on its lease (NAN without one) into *lease_seconds. Returns the name's full length,
        // 0 when nobody holds the camera (an expired lease reads as nobody).
        uint32_t (*camera_owner)(char* name, uint32_t capacity, double* lease_seconds);
    };
} // namespace dwcam

// The table, or null for a version this core does not serve (0, or newer than API_VERSION). Any thread.
extern "C" const dwcam::Api* dwcc_get_api(uint32_t version);
