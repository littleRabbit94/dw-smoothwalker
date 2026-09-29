// The camera core's hook side (docs/design.md, "Core and processors"): the view update count and world seconds,
// cuts, the crossfade, layers, the write and the API snapshot, the processor and listener slots, and CoreApi, the
// CameraCore over them (camera/api.hpp). Nothing here calls a UObject or UE4SS: the hook runs on task-graph
// workers. The hook itself, slot 214 and the image-level statics are camera/hook.cpp's; the API's owner and layers
// are the Authority's (camera/authority.hpp).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "../common/log.hpp"
#include "../common/math.hpp"
#include "api.hpp"
#include "clock.hpp"
#include "frame.hpp"
#include "guarded.hpp"
#include "snapshot.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dw::camera
{
    class Authority;

    // Prefix of UE 5.5 FMinimalViewInfo: Location, Rotation, FOV.
    struct ViewHead
    {
        double location[3];
        double rotation[3];
        float fov;
    };
    constexpr size_t VIEW_BYTES = offsetof(ViewHead, fov) + sizeof(float); // stops at FOV: not the padding, not DesiredFOV

    // The core's own settings: the cut thresholds (CameraCore::set_cut_thresholds). The crossfade's length is the
    // processor's (FrameOut::transition). Numbers only, so the hook's copy allocates nothing on a worker thread.
    struct Tuning
    {
        double reset_distance, reset_gap;
        uint64_t generation; // bumped by a push that changed a value
    };
    constexpr Tuning DEFAULT_TUNING{500.0, 0.25, 0}; // smoothwalker.ini's shipped reset_distance and reset_gap

    // Hook state without a lock: only the player's camera reaches it, and its calls arrive in sequence. The core's
    // part of each update (cuts, ownership, the crossfade); the processor keeps its own.
    struct ViewState
    {
        bool valid = false; // false: the processor starts again from the capsule on the next update (a restart)
        dw::Vec3 pivot_last{};
        LARGE_INTEGER last_call{};

        // Last view handed to the game, relative to the game's own view that frame. The arm is rebuilt from the
        // game's camera each frame, so a fade follows moving and turning even with rotation smoothing on.
        bool out_valid = false;
        dw::Vec3 out_offset{};     // shown lag: pivot + arm under the output rotation - output location
        dw::Quat out_rotation{}; // output rotation * inverse(game rotation)
        float out_fov = NAN;

        bool blending = false;
        double blend_elapsed = 0.0, blend_duration = 0.0;
        dw::Vec3 from_offset{};
        dw::Quat from_rotation{};
        float from_fov = NAN;
        uint64_t seen_tuning = 0, seen_processor = 0, seen_toggle = 0, seen_release = 0;
        bool was_owned = false; // another mod owned the camera on the last update: the falling edge is a cut
        Snap invalid_reason = Snap::Startup; // why valid went false, for the debug overlay's last snap
        bool blend_glide = false;                         // the running crossfade came from a release("glide")
    };

    // Counts a caller in for as long as it lives (the processor and listener slots).
    struct Counted
    {
        std::atomic<int>& count;
        explicit Counted(std::atomic<int>& c) : count(c) { count.fetch_add(1); }
        ~Counted() { count.fetch_sub(1); }
        Counted(const Counted&) = delete;
        auto operator=(const Counted&) -> Counted& = delete;
    };

    class Pipeline
    {
      public:
        // `authority`: the API state the hook reads and writes (the owner, layers, the blending flag); only bound
        // here, so the two may be constructed in either order. `clock`: every stamp, gap and age is read on it.
        // `copy`: every read of the player's objects and the write of the view go through it. The Core passes
        // QPC_CLOCK and seh_copy; a test its own.
        Pipeline(Authority& authority, Clock clock, GuardedCopy copy) : m_authority(authority), m_clock(clock), m_copy(copy) {}
        Pipeline(const Pipeline&) = delete;
        auto operator=(const Pipeline&) -> Pipeline& = delete;

        // The hook (camera/hook.cpp), after the game's own GetCameraView: the rest of one camera update. Returns at
        // once for any camera but the player's.
        auto on_camera_view(void* self, float delta_time, void* desired_view) -> void;

        // ------------------------------------------------------------------------ game thread (camera/core.cpp)

        // A hard cut on the next camera update, and why. The first reason since the hook last took a cut is kept: a
        // level change is followed by a new controller and a new pawn, and the level change is the one to show. A
        // hook taking the cut between the load and the stores leaves this cut with the older reason (display only).
        // release("cut") names its own reason (Authority::release_locked).
        auto request_cut(Snap why) -> void
        {
            if (!m_reset.load()) m_reset_reason.store(static_cast<int>(why));
            m_reset.store(true);
        }
        // Published by the game thread, read by the hook. Pointers are only compared or read under SEH.
        auto set_player_camera(void* camera) -> void { m_player_camera.store(camera); }
        auto set_player_root(void* root) -> void { m_player_root.store(root); }
        // The controller held (core.cpp keeps it with its LiveRef), and whether there is one.
        auto set_player_controller(void* controller) -> void
        {
            m_player_controller.store(controller);
            m_player_known.store(controller != nullptr);
        }
        auto translation_offset() const -> int32_t { return m_translation_offset.load(); }
        auto set_translation_offset(int32_t offset) -> void { m_translation_offset.store(offset); }
        auto half_height_offset() const -> int32_t { return m_half_height_offset.load(); }
        auto set_half_height_offset(int32_t offset) -> void { m_half_height_offset.store(offset); }
        // The game thread's id, captured on the engine tick; 0 before the first. Any thread reads it.
        auto game_thread() const -> uint32_t { return m_game_thread.load(std::memory_order_relaxed); }
        auto set_game_thread(uint32_t id) -> void { m_game_thread.store(id); }

        // Game thread: a listener callback, if one is set.
        template <typename Call>
        auto notify(Call&& call) -> void
        {
            Counted in_listener(m_in_listener);
            if (Listener* listener = m_listener.load()) call(*listener);
        }

        // Lua's Smoothwalker.enabled() (Authority::enabled): the processor's switch, false without one.
        auto processor_enabled() -> bool;

        // ----------------------------------------------------------------------- CameraCore (CoreApi forwards)

        auto register_processor(Processor& p) -> bool;
        auto unregister_processor(Processor& p) -> bool;
        auto set_listener(Listener& l) -> bool;
        auto clear_listener(Listener& l) -> bool;
        auto set_cut_thresholds(double reset_distance, double reset_gap) -> void;
        auto set_diagnostics(uint32_t flags) -> void;
        auto player_camera() const -> void* { return m_player_camera.load(std::memory_order_relaxed); }
        auto player_controller() const -> void* { return m_player_controller.load(std::memory_order_relaxed); }
        auto player_known() const -> bool { return m_player_known.load(); }
        auto view_updates() const -> uint64_t { return m_view_updates.load(); }
        auto view_seconds() const -> double { return m_view_seconds.load(); }
        // False under a pause, a load, a cutscene or the free camera: the player's camera is not updating.
        auto camera_live() const -> bool;
        auto take_hook_timing(uint64_t& calls, double& microseconds_per_call) -> void;
        auto read_debug() const -> DebugFeed;

        // ------------------------------------------------------------------------------ the Authority's reads

        // The hard-cut flag and its reason (release("cut") snaps through the same flag a teleport sets), and the
        // view snapshot (Lua's view() and live(), claim()'s fade check).
        auto reset_flag() -> std::atomic<bool>& { return m_reset; }
        auto reset_reason() -> std::atomic<int>& { return m_reset_reason; }
        auto snapshot() const -> const ViewSnapshot& { return m_snapshot; }

        // Read-only copy of what the hook publishes, for a single-threaded check (the equivalence harness).
        struct Inspect
        {
            double keep_follow, keep_turn;
            int influence;
            double lag_h, lag_v, rate_h;
            int snap;
            int64_t snap_qpc;
            bool glide;
            uint64_t view_updates;
            double view_seconds;
            int64_t last_view_qpc;
            bool reset;
            int reset_reason;
        };
        auto inspect() const -> Inspect;

      private:
        auto seconds_between(LARGE_INTEGER a, LARGE_INTEGER b) const -> double;
        auto publish_api_view(const ViewHead& game, const ViewHead& shown, const dw::Vec3* pivot) -> void;
        auto publish_debug_idle() -> void;
        auto lose_view() -> void;
        auto update_view(void* desired_view, float delta_time, Processor* processor, bool enabled, uint64_t toggle) -> void;

        Authority& m_authority;
        Clock m_clock;
        GuardedCopy m_copy;
        std::atomic<uint32_t> m_game_thread{0};

        SRWLOCK m_tuning_lock = SRWLOCK_INIT; // m_tuning
        Tuning m_tuning = DEFAULT_TUNING;

        std::atomic<bool> m_reset{true}; // a hard cut: snap, no crossfade
        // Why m_reset was set; stored before it (request_cut, Authority::release_locked).
        std::atomic<int> m_reset_reason{static_cast<int>(Snap::Startup)};
        std::atomic<bool> m_hook_timing{false}; // DIAG_HOOK_TIMING: log_stats times the hook
        std::atomic<void*> m_player_camera{nullptr};
        std::atomic<void*> m_player_root{nullptr};
        std::atomic<void*> m_player_controller{nullptr}; // game thread: the controller held
        std::atomic<bool> m_player_known{false};         // a player controller is held; read by Smoothwalker's banners
        std::atomic<int32_t> m_translation_offset{-1}; // USceneComponent::ComponentToWorld.Translation
        std::atomic<int32_t> m_half_height_offset{-1};  // UCapsuleComponent::CapsuleHalfHeight; -1: the processor tracks the centre

        std::atomic<uint64_t> m_view_updates{0}; // player-camera updates: Smoothwalker's flip stages advance on these
        std::atomic<int64_t> m_last_view_qpc{0};  // camera_live(): preset and shoulder keys act only while this is recent
        std::atomic<double> m_view_seconds{0.0};  // world time over those updates: the flip's glide runs on it
        std::atomic<uint64_t> m_calls_timed{0};
        std::atomic<uint64_t> m_ticks_spent{0};

        // The debug overlay's feed (CameraCore::read_debug): written by the hook on the player's camera updates, read
        // on the game thread at the overlay's refresh. Numbers only and relaxed, so a refresh may pair values from two
        // frames. NAN: not following (off, or the view was lost).
        std::atomic<double> m_debug_keep_follow{NAN}; // share of the trail shown after the traversal, combat and aiming blends
        std::atomic<double> m_debug_keep_turn{NAN};   // share of the turning smoothing shown
        std::atomic<int> m_debug_influence{0};        // follow::Influence: the largest weight in those blends
        std::atomic<double> m_debug_lag_h{NAN};       // cm of lag on screen (out_offset), horizontal
        std::atomic<double> m_debug_lag_v{NAN};       // and vertical
        std::atomic<double> m_debug_rate_h{NAN};      // 1/s, the horizontal follow rate after the curve at the current lag
        std::atomic<int> m_debug_snap{0};             // Snap of the last restart from the capsule
        std::atomic<int64_t> m_debug_snap_qpc{0};     // QPC of it; 0: none yet
        std::atomic<bool> m_debug_glide{false};       // the crossfade running was started by a release("glide")

        // The processor and listener slots (CameraCore::register_processor, CameraCore::set_listener). A caller counts
        // itself in before it loads the slot, so unregistering (store null, then wait for 0) never returns while a call
        // into the old one is in flight.
        std::atomic<Processor*> m_processor{nullptr};
        std::atomic<Listener*> m_listener{nullptr};
        std::atomic<int> m_in_processor{0}; // the hook and Lua's enabled() inside m_processor
        std::atomic<int> m_in_listener{0};  // the game thread inside m_listener

        ViewState m_view;
        ViewSnapshot m_snapshot; // the API snapshot: published here, read by Lua's view() and live() through the Authority
    };

    // The core's interface (camera/api.hpp) over one Pipeline and its Authority, for Core::api() and the equivalence
    // harness: forwards only, no state of its own. Any thread, as each method says.
    class CoreApi final : public CameraCore
    {
      public:
        CoreApi(Pipeline& pipeline, const Authority& authority) : m_pipeline(pipeline), m_authority(authority) {}

        auto register_processor(Processor& p) -> bool override;
        auto unregister_processor(Processor& p) -> bool override;
        auto set_listener(Listener& l) -> bool override;
        auto clear_listener(Listener& l) -> bool override;
        auto set_cut_thresholds(double reset_distance, double reset_gap) -> void override;
        auto set_diagnostics(uint32_t flags) -> void override;
        auto player_camera() const -> RC::Unreal::UObject* override;
        auto player_controller() const -> RC::Unreal::UObject* override;
        auto player_known() const -> bool override;
        auto view_updates() const -> uint64_t override;
        auto view_seconds() const -> double override;
        auto camera_live() const -> bool override;
        auto game_thread_id() const -> uint32_t override;
        auto take_hook_timing(uint64_t& calls, double& microseconds_per_call) -> void override;
        auto read_debug() const -> DebugFeed override;
        auto camera_owner() const -> Owner override;

      private:
        Pipeline& m_pipeline;
        const Authority& m_authority;
    };
} // namespace dw::camera
