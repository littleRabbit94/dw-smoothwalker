// The camera core's C++ interface (docs/design.md, "Core and processors"): the only way Smoothwalker's side reaches
// the core. CameraCore is what the core hands out (Core::api()); Processor and Listener are what a registrant
// implements and hands in. All three live in the one DLL, and nothing is deleted through them: the core owns no
// processor or listener, a registrant does not own the core. Per-frame data crosses as the numbers-only structs of
// camera/frame.hpp; UObjects only as pointers, declared here and never dereferenced by this header.
#pragma once

#include "frame.hpp"

#include <cmath>
#include <cstdint>
#include <string>

namespace RC::Unreal
{
    class UObject;
}

namespace dw::camera
{
    // A view processor. The core runs at most one (register_processor answers false while one is registered) and
    // calls nothing of it while none is: the hook then leaves the game's view alone unless a layer is set. Owned by
    // its registrant, which keeps it alive until it is unregistered.
    class Processor
    {
      public:
        // Hook thread (a task-graph worker, never the game thread), once per player-camera update that reaches the
        // pipeline, in sequence, whether the processor is on or off (off: bookkeeping only). Numbers only: no
        // UObject, no UE4SS, no lock that a game-thread call can hold for long, no allocation. `out` is
        // value-initialized before the call.
        virtual auto frame(const FrameIn& in, FrameOut& out) -> void = 0;
        // Any thread (the hook once per player-camera update, before anything else; Lua's enabled() anywhere),
        // lock-free: true while the processor is on. Writes its toggle generation, which the processor bumps on every
        // switch before the switch itself, to toggle_generation. The hook's "off and settled" path is decided on
        // these two alone, so an off processor with nothing left to fade costs one call.
        virtual auto state(uint64_t& toggle_generation) const -> bool = 0;

      protected:
        Processor() = default;
        ~Processor() = default;
        Processor(const Processor&) = default;
        auto operator=(const Processor&) -> Processor& = default;
    };

    // Game-thread notifications for the processor's owner, called from the core's own UE4SS callbacks at fixed
    // points, so its game-thread work keeps one order against player discovery. Each defaults to nothing.
    class Listener
    {
      public:
        // Game thread: a level change (LoadMap, or the engine tick seeing a new world). The player is already
        // forgotten; every UObject pointer taken in the old world goes. May come twice for one load.
        virtual auto world_changed() -> void {}
        // Game thread: a new player camera was just published (player_camera()).
        virtual auto camera_changed() -> void {}
        // Game thread, every engine tick: after the player controller is checked and discovered, before the pawn and
        // its camera are.
        virtual auto tick() -> void {}

      protected:
        Listener() = default;
        ~Listener() = default;
        Listener(const Listener&) = default;
        auto operator=(const Listener&) -> Listener& = default;
    };

    // set_diagnostics flags.
    constexpr uint32_t DIAG_VERBOSE = 1u;     // log_verbose: the one verbose flag (common/log.hpp)
    constexpr uint32_t DIAG_HOOK_TIMING = 2u; // log_stats: time the hook (take_hook_timing)

    // The debug overlay's feed as the hook left it (read_debug). Relaxed reads: one call may pair values from two
    // updates.
    struct DebugFeed
    {
        double keep_follow, keep_turn;    // the processor's feed (FrameOut) as last reported; NAN: not following
        double rate_h;                    // likewise
        int32_t influence;                // likewise, follow::Influence
        double lag_h, lag_v;              // cm of lag on screen, horizontal and vertical; NAN: not following
        int32_t snap;                     // Snap of the last restart from the capsule
        double snap_age;                  // s since it; NAN: none yet
        bool glide;                       // the running crossfade was started by a release("glide")
    };

    // The camera owner as Smoothwalker.owner() reads it (camera_owner).
    struct Owner
    {
        std::string mod;    // empty: nobody holds the camera (an expired lease reads as nobody)
        double lease = NAN; // s left on the owner's lease; NAN without one
    };

    class CameraCore
    {
      public:
        // Game thread or the mod's start thread, never the hook: true registered, false when one is already
        // registered. The core keeps a pointer: p must stay alive until unregistered.
        virtual auto register_processor(Processor& p) -> bool = 0;
        // Game thread or the mod's start thread, never inside a processor call: returns once no call into p is in
        // flight (up to 5 s). True: drained, or p was not the one registered (nothing could be calling it). False:
        // timed out, a call may still be running inside p, so the caller must not free anything the calls reach.
        virtual auto unregister_processor(Processor& p) -> bool = 0;
        // Game thread or the mod's start thread: true set, false when one is set already. Kept like a processor.
        virtual auto set_listener(Listener& l) -> bool = 0;
        // Game thread or the mod's start thread, never inside a listener call: returns once no call into l is in
        // flight (up to 5 s). Same result as unregister_processor: true drained or not the one set, false timed out.
        virtual auto clear_listener(Listener& l) -> bool = 0;

        // Any thread but the hook (takes the core's settings lock exclusively): the cut thresholds, cm moved in one
        // update that counts as a teleport and s without an update that counts as a gap. A change crossfades like a
        // processor's generation; non-finite values are ignored. The core starts at 500 cm and 0.25 s.
        virtual auto set_cut_thresholds(double reset_distance, double reset_gap) -> void = 0;
        // Any thread: DIAG_* flags, relaxed.
        virtual auto set_diagnostics(uint32_t flags) -> void = 0;

        // Any thread: the player's camera component, or null. Compare it anywhere; dereference it on the game thread only.
        virtual auto player_camera() const -> RC::Unreal::UObject* = 0;
        // Game thread: the player controller, checked live on this engine tick, or null.
        virtual auto player_controller() const -> RC::Unreal::UObject* = 0;
        // Any thread: true while a player controller is held.
        virtual auto player_known() const -> bool = 0;
        // Any thread: player-camera updates since the core started.
        virtual auto view_updates() const -> uint64_t = 0;
        // Any thread: world seconds summed over those updates (their DeltaTime).
        virtual auto view_seconds() const -> double = 0;
        // Any thread: true if the player's camera updated within the last 0.25 s (not paused, loading, in a cutscene
        // or a free camera).
        virtual auto camera_live() const -> bool = 0;
        // Any thread: the game thread's id, 0 before the first engine tick.
        virtual auto game_thread_id() const -> uint32_t = 0;

        // Any thread: hook calls timed since the last take and their mean cost in microseconds (0 with no calls),
        // then both counters start again. Timing runs only under DIAG_HOOK_TIMING.
        virtual auto take_hook_timing(uint64_t& calls, double& microseconds_per_call) -> void = 0;
        // Any thread: the debug overlay's feed (see DebugFeed).
        virtual auto read_debug() const -> DebugFeed = 0;
        // Any thread (takes the Lua API's mutex briefly): the camera owner's mod name and the s left on its lease.
        virtual auto camera_owner() const -> Owner = 0;

      protected:
        CameraCore() = default;
        ~CameraCore() = default;
        CameraCore(const CameraCore&) = default;
        auto operator=(const CameraCore&) -> CameraCore& = default;
    };
} // namespace dw::camera
