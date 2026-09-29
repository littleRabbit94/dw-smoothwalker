// The camera core's hook side (docs/design.md, "Core and processors"): the GetCameraView hook on vtable slot 214,
// the view update count and world seconds, cuts, the crossfade, layers, the write and the API snapshot, the
// processor and listener slots, and the C table over them (camera/api.hpp). Nothing here calls a UObject or UE4SS:
// the hook runs on task-graph workers. Included by camera/core.cpp only (and the equivalence harness,
// tests/equivalence), so its globals live in that one translation unit.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "../common/math.hpp"
#include "api.hpp"
#include "frame.hpp"
#include "lua_api.hpp"
#include "wall.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace dw::camera
{
namespace
{
    constexpr size_t GET_CAMERA_VIEW_SLOT = 214;

    // Prefix of UE 5.5 FMinimalViewInfo: Location, Rotation, FOV.
    struct ViewHead
    {
        double location[3];
        double rotation[3];
        float fov;
    };
    constexpr size_t VIEW_BYTES = offsetof(ViewHead, fov) + sizeof(float); // stops at FOV: not the padding, not DesiredFOV

    // The API snapshot (lua_api.hpp): the game's view, what was handed back, and the pivot. Hook thread, numbers only.
    auto publish_api_view(const ViewHead& game, const ViewHead& shown, const dw::Vec3* pivot) -> void
    {
        lua::View g{{game.location[0], game.location[1], game.location[2]}, {game.rotation[0], game.rotation[1], game.rotation[2]}, game.fov};
        lua::View s{{shown.location[0], shown.location[1], shown.location[2]}, {shown.rotation[0], shown.rotation[1], shown.rotation[2]}, shown.fov};
        double p[3]{};
        if (pivot) { p[0] = pivot->x; p[1] = pivot->y; p[2] = pivot->z; }
        lua::publish(g, s, pivot ? p : nullptr);
    }

    // The core's own settings: the cut thresholds (Api::set_cut_thresholds). The crossfade's length is the
    // processor's (FrameOut::transition). Numbers only, so the hook's copy allocates nothing on a worker thread.
    struct Tuning
    {
        double reset_distance, reset_gap;
        uint64_t generation; // bumped by a push that changed a value
    };
    constexpr Tuning DEFAULT_TUNING{500.0, 0.25, 0}; // smoothwalker.ini's shipped reset_distance and reset_gap

    using GetCameraViewFn = void(__fastcall*)(void* self, float delta_time, void* desired_view);

    // Kept across a hot reload on purpose (reset_globals): install_hook reads all three on the reused image.
    GetCameraViewFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;
    bool g_hook_left = false; // the last unload found another mod's hook over ours and left the slot alone
    std::atomic<int> g_in_hook{0}; // calls running inside get_camera_view_hook; unload waits for 0
    int g_starts = 0;              // constructions in this process: one image, pinned (pin_module)

    SRWLOCK g_tuning_lock = SRWLOCK_INIT; // g_tuning
    Tuning g_tuning = DEFAULT_TUNING;

    // Published by the game thread, read by the hook. Pointers are only compared or read under SEH.
    std::atomic<bool> g_reset{true}; // a hard cut: snap, no crossfade
    std::atomic<int> g_reset_reason{static_cast<int>(dw::Snap::Startup)}; // why g_reset was set; stored before it (request_cut, lua_api.hpp)
    std::atomic<bool> g_hook_timing{false}; // DIAG_HOOK_TIMING: log_stats times the hook
    std::atomic<bool> g_log_verbose{false}; // DIAG_VERBOSE: the core's verbose log lines
    std::atomic<void*> g_player_camera{nullptr};
    std::atomic<void*> g_player_root{nullptr};
    std::atomic<void*> g_player_controller{nullptr}; // game thread: the controller held (core.cpp keeps it with its LiveRef)
    std::atomic<bool> g_player_known{false};         // a player controller is held; read by Smoothwalker's banners
    std::atomic<int32_t> g_translation_offset{-1}; // USceneComponent::ComponentToWorld.Translation
    std::atomic<int32_t> g_half_height_offset{-1};  // UCapsuleComponent::CapsuleHalfHeight; -1: the processor tracks the centre

    std::atomic<uint64_t> g_view_updates{0}; // player-camera updates: Smoothwalker's flip stages advance on these
    std::atomic<int64_t> g_last_view_qpc{0};  // camera_live(): preset and shoulder keys act only while this is recent
    std::atomic<double> g_view_seconds{0.0};  // world time over those updates: the flip's glide runs on it
    std::atomic<uint64_t> g_calls_timed{0};
    std::atomic<uint64_t> g_ticks_spent{0};

    // The debug overlay's feed (Api::read_debug): written by the hook on the player's camera updates, read on the
    // game thread at the overlay's refresh. Numbers only and relaxed, so a refresh may pair values from two frames.
    // NAN: not following (off, or the view was lost).
    std::atomic<double> g_debug_keep_follow{NAN}; // share of the trail shown after the traversal, combat and aiming blends
    std::atomic<double> g_debug_keep_turn{NAN};   // share of the turning smoothing shown
    std::atomic<int> g_debug_influence{0};        // dw::Influence: the largest weight in those blends
    std::atomic<double> g_debug_lag_h{NAN};       // cm of lag on screen (out_offset), horizontal
    std::atomic<double> g_debug_lag_v{NAN};       // and vertical
    std::atomic<double> g_debug_rate_h{NAN};      // 1/s, the horizontal follow rate after the curve at the current lag
    std::atomic<int> g_debug_snap{0};             // dw::Snap of the last restart from the capsule
    std::atomic<int64_t> g_debug_snap_qpc{0};     // QPC of it; 0: none yet
    std::atomic<bool> g_debug_glide{false};       // the crossfade running was started by a release("glide")

    // The processor and listener slots (Api::register_processor, Api::set_listener). A caller counts itself in
    // before it loads the slot, so unregistering (store null, then wait for 0) never returns while a call into the
    // old one is in flight. g_in_processor and g_in_listener balance themselves and are kept across a hot reload.
    std::atomic<const Processor*> g_processor{nullptr};
    std::atomic<const Listener*> g_listener{nullptr};
    std::atomic<int> g_in_processor{0}; // the hook and Lua's enabled() inside g_processor
    std::atomic<int> g_in_listener{0};  // the game thread inside g_listener

    struct Counted
    {
        std::atomic<int>& count;
        explicit Counted(std::atomic<int>& c) : count(c) { count.fetch_add(1); }
        ~Counted() { count.fetch_sub(1); }
        Counted(const Counted&) = delete;
        auto operator=(const Counted&) -> Counted& = delete;
    };

    // Game thread: a hard cut on the next camera update, and why. The first reason since the hook last took a cut is
    // kept: a level change is followed by a new controller and a new pawn, and the level change is the one to show.
    // A hook taking the cut between the load and the stores leaves this cut with the older reason (display only).
    // release("cut") names its own reason (lua_api.hpp).
    auto request_cut(dw::Snap why) -> void
    {
        if (!g_reset.load()) g_reset_reason.store(static_cast<int>(why));
        g_reset.store(true);
    }

    // Kept free of C++ objects: __try needs a plain frame.
    auto guarded_read(void* from, void* to, size_t bytes) -> bool
    {
        __try
        {
            memcpy(to, from, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto guarded_write(void* to, const void* from, size_t bytes) -> bool
    {
        __try
        {
            memcpy(to, from, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

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
        dw::Snap invalid_reason = dw::Snap::Startup; // why valid went false, for the debug overlay's last snap
        bool blend_glide = false;                         // the running crossfade came from a release("glide")
    };
    ViewState g_view;
    LARGE_INTEGER g_qpc_frequency{};

    auto seconds_between(LARGE_INTEGER a, LARGE_INTEGER b) -> double
    {
        return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g_qpc_frequency.QuadPart);
    }

    auto finite(const dw::Vec3& v) -> bool
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // The debug overlay's feed while nothing is followed.
    auto publish_debug_idle() -> void
    {
        g_debug_keep_follow.store(NAN, std::memory_order_relaxed);
        g_debug_keep_turn.store(NAN, std::memory_order_relaxed);
        g_debug_lag_h.store(NAN, std::memory_order_relaxed);
        g_debug_lag_v.store(NAN, std::memory_order_relaxed);
        g_debug_rate_h.store(NAN, std::memory_order_relaxed);
        g_debug_glide.store(false, std::memory_order_relaxed);
    }

    auto lose_view() -> void
    {
        g_view.valid = false;
        g_view.invalid_reason = dw::Snap::ViewLost;
        g_view.out_valid = false;
        g_view.blending = false;
        lua::g_blending.store(false, std::memory_order_relaxed);
        publish_debug_idle();
    }

    // The core's pipeline, once per player-camera update: read the game's view and the pivot, work out cuts and
    // ownership, let the processor move the camera, crossfade any change, apply other mods' layers, write the view
    // back and publish it (docs/design.md, "Core and processors"). processor, enabled and toggle: the hook's one
    // sample of the processor slot and its switch for this update.
    auto update_view(void* desired_view, float delta_time, const Processor* processor, bool enabled, uint64_t toggle) -> void
    {
        // Ownership is sampled once, first, and that one sample is used for the whole update. A release runs on the
        // game thread while this runs on a worker: sampling g_owner_slot after g_release_generation and g_reset
        // could see the release already published and the camera still owned, or the other way round, and write a
        // full follow offset for one frame, which turns a glide into a cut and a cut into a double snap. A lease
        // that has run out is not a claim; the game thread drops the identity the next time it looks (lua_api.hpp).
        // The slot is read before the lease, so a fresh claim never pairs with the previous owner's stale expiry.
        const bool owner_held = lua::g_owner_slot.load(std::memory_order_acquire) >= 0;
        const int64_t owner_expires = lua::g_owner_expires.load(std::memory_order_relaxed);
        const bool owned = owner_held && (owner_expires == 0 || lua::qpc_now() < owner_expires);
        const bool owner_keeps_layers = owned && lua::g_owner_keep_layers.load(std::memory_order_relaxed);
        // The release generation, sampled here because the falling edge below needs it; `changed` uses this sample.
        const auto release = lua::g_release_generation.load(std::memory_order_relaxed);
        const bool release_changed = release != g_view.seen_release;
        // The camera stops being owned. A release("cut") has already set g_reset, and a release("glide") has bumped
        // the generation just read, which starts the crossfade from out_*, the game's view as the owner left it. A
        // lease running out, an uninstall or an install re-key say nothing, and writing the follow offset that piled
        // up while owned would pop the camera in that one frame and pop it again when the game thread later notices
        // and sets g_reset: those edges restart the follow from the capsule here, which is what a cut does. A glide
        // is left alone, because resetting would throw away the warm follow it is meant to ease back into and the
        // crossfade would run from nothing to nothing.
        if (g_view.was_owned && !owned && !release_changed)
        {
            g_view.valid = false;
            g_view.invalid_reason = dw::Snap::ClaimEnded;
        }
        g_view.was_owned = owned;

        AcquireSRWLockShared(&g_tuning_lock);
        const Tuning t = g_tuning;
        ReleaseSRWLockShared(&g_tuning_lock);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        auto* root = g_player_root.load(std::memory_order_relaxed);
        auto offset = g_translation_offset.load(std::memory_order_relaxed);
        double pivot_raw[3]{};
        ViewHead view{};
        if (!root || offset < 0 || !guarded_read(static_cast<uint8_t*>(root) + offset, pivot_raw, sizeof(pivot_raw)) ||
            !guarded_read(desired_view, &view, VIEW_BYTES))
        {
            lose_view();
            return;
        }

        const ViewHead game_view = view; // for the API snapshot
        dw::Vec3 pivot{pivot_raw[0], pivot_raw[1], pivot_raw[2]};
        dw::Vec3 camera{view.location[0], view.location[1], view.location[2]};
        if (!finite(pivot) || !finite(camera) || !std::isfinite(view.rotation[0]) || !std::isfinite(view.rotation[1]) ||
            !std::isfinite(view.rotation[2]))
        {
            lose_view();
            return;
        }
        dw::Quat rotation = dw::from_rotator(view.rotation[0], view.rotation[1], view.rotation[2]);

        // The capsule's half height, for a processor that tracks the feet (the follow, "Pivot" in docs/design.md).
        // NAN when missing or implausible.
        float half_height = NAN;
        auto half_offset = g_half_height_offset.load(std::memory_order_relaxed);
        if (half_offset >= 0 && !guarded_read(static_cast<uint8_t*>(root) + half_offset, &half_height, sizeof(half_height))) half_height = NAN;
        if (!std::isfinite(half_height) || half_height < 0.0f || half_height > 1000.0f) half_height = NAN;

        // The cut thresholds, the processor's switch and settings, and its mode writes crossfade; a hard cut (player
        // or world change, a gap, a teleport) snaps. A release("glide") lands here too, through the `release_changed`
        // sampled at the top of this update: the fade then starts from the game's view, because out_* tracked it
        // while the camera was owned (lua_api.hpp). The processor's generation joins below, with the frame it
        // reports it for.
        bool changed = t.generation != g_view.seen_tuning || toggle != g_view.seen_toggle || release_changed;
        g_view.seen_tuning = t.generation;
        g_view.seen_toggle = toggle;
        g_view.seen_release = release;
        const bool reset = g_reset.exchange(false, std::memory_order_relaxed);
        const bool gap = seconds_between(g_view.last_call, now) > t.reset_gap;
        const bool jump = !(dw::length(pivot - g_view.pivot_last) <= t.reset_distance);
        bool cut = reset || gap || jump;
        g_view.last_call = now;
        g_view.pivot_last = pivot;

        // The world delta, so slow motion slows the follow and the crossfade with the game. A pause is not
        // held: the camera stops updating under one, and the first update after it is a cut (reset_gap).
        double dt = std::clamp(static_cast<double>(delta_time), 0.0, 0.1);

        bool restart = false;
        if (!enabled)
        {
            g_view.valid = false; // switched back on, the follow restarts from the capsule
            g_view.invalid_reason = dw::Snap::Toggle;
            g_debug_keep_follow.store(NAN, std::memory_order_relaxed);
            g_debug_keep_turn.store(NAN, std::memory_order_relaxed);
            g_debug_rate_h.store(NAN, std::memory_order_relaxed);
        }
        else if (cut || !g_view.valid)
        {
            // Why, for the debug overlay: switched back on beats a cut still pending from while it was off; then
            // whoever asked for the cut; then why the follow was dropped; then the hook's own gap and teleport checks.
            auto why = !g_view.valid && g_view.invalid_reason == dw::Snap::Toggle ? dw::Snap::Toggle
                       : reset                                                    ? static_cast<dw::Snap>(g_reset_reason.load(std::memory_order_relaxed))
                       : !g_view.valid                                            ? g_view.invalid_reason
                       : gap                                                      ? dw::Snap::Gap
                                                                                  : dw::Snap::Teleport;
            g_debug_snap_qpc.store(now.QuadPart, std::memory_order_relaxed);
            g_debug_snap.store(static_cast<int>(why), std::memory_order_relaxed);
            g_view.valid = true;
            restart = true;
        }

        FrameIn in{};
        in.size = sizeof(FrameIn);
        in.enabled = enabled;
        in.restart = restart;
        in.dt = dt;
        in.pivot = pivot;
        in.half_height = static_cast<double>(half_height);
        in.camera = camera;
        in.rotation = rotation;
        FrameOut moved{};
        if (processor) processor->frame(processor->user, &in, &moved);
        if (moved.generation != g_view.seen_processor) changed = true;
        g_view.seen_processor = moved.generation;

        dw::Vec3 result = camera;
        dw::Quat result_rotation = rotation;
        if (enabled)
        {
            result = moved.location;
            if (moved.rotated)
            {
                result_rotation = moved.rotation;
                dw::to_rotator(result_rotation, view.rotation[0], view.rotation[1], view.rotation[2]);
            }
        }
        if (moved.feed)
        {
            g_debug_keep_follow.store(moved.keep_follow, std::memory_order_relaxed);
            g_debug_keep_turn.store(moved.keep_turn, std::memory_order_relaxed);
            g_debug_rate_h.store(moved.rate_h, std::memory_order_relaxed);
            g_debug_influence.store(moved.influence, std::memory_order_relaxed);
        }

        // Another mod owns the camera (lua_api.hpp, "authority"; `owned` was sampled at the top of this update).
        // The processor above ran and its state stays warm, but nothing of it is shown: the view goes back to
        // exactly what the game built, so out_* below record a zero offset, an identity rotation and the game's FOV
        // and a later release("glide") starts from there. No crossfade runs while owned either; the release decides
        // how the camera comes back.
        if (owned)
        {
            view = game_view;
            result = camera;
            result_rotation = rotation;
        }

        // A new lag limit, rotation smoothing or FOV would otherwise jump when it lands mid-motion.
        const double transition = moved.transition;
        bool fov_ok = std::isfinite(view.fov) && view.fov > 1.0f && view.fov < 179.0f;
        if (cut || owned)
        {
            g_view.blending = false;
        }
        else if (transition <= 0.0)
        {
            g_view.blending = false; // set to 0 mid-fade: the user's choice is no fade
        }
        else if (changed && g_view.out_valid)
        {
            g_view.blending = true;
            g_view.blend_glide = release_changed;
            g_view.blend_elapsed = 0.0;
            g_view.blend_duration = transition;
            g_view.from_offset = g_view.out_offset;
            g_view.from_rotation = g_view.out_rotation;
            g_view.from_fov = g_view.out_fov;
        }
        bool blended = g_view.blending;
        if (blended)
        {
            g_view.blend_elapsed += dt;
            double s = std::min(g_view.blend_elapsed / g_view.blend_duration, 1.0);
            double w = s * s * (3.0 - 2.0 * s);
            dw::Vec3 game_arm = camera - pivot;
            dw::Quat target = dw::multiply(result_rotation, dw::conjugate(rotation));
            dw::Quat d = dw::slerp(g_view.from_rotation, target, w);
            dw::Vec3 lag = pivot + dw::rotate(target, game_arm) - result;
            result = pivot - (g_view.from_offset + (lag - g_view.from_offset) * w) + dw::rotate(d, game_arm);
            result_rotation = dw::multiply(d, rotation);
            dw::to_rotator(result_rotation, view.rotation[0], view.rotation[1], view.rotation[2]);
            if (fov_ok && std::isfinite(g_view.from_fov)) view.fov = g_view.from_fov + static_cast<float>((view.fov - g_view.from_fov) * w);
            if (s >= 1.0) g_view.blending = false;

            // The faded part of the lag was never clamped: keep it in front of a wall the game pulled in for.
            double game_distance = dw::length(game_arm);
            // The toggle-off fade too: it ends at the game's view, so clamping to the game's distance never moves the endpoint.
            if ((!enabled || g_view.valid) && moved.wall_clamp)
            {
                clamp_to_wall(result, pivot, game_distance, wall_weight(game_distance, moved.nominal_distance));
            }
        }

        bool apply_result = (enabled && !owned) || blended;
        if (apply_result)
        {
            if (!finite(result) || !std::isfinite(view.rotation[0]) || !std::isfinite(view.rotation[1]) || !std::isfinite(view.rotation[2]))
            {
                lose_view(); // leave the game's view as it built it
                return;
            }
            view.location[0] = result.x;
            view.location[1] = result.y;
            view.location[2] = result.z;
        }
        // Other mods' layers (lua_api.hpp), on top of whatever the processor did, its switch included. While another
        // mod owns the camera they are off too, unless that owner asked to keep them.
        bool layered = (!owned || owner_keeps_layers) && lua::apply_layers(view.location, view.rotation, view.fov, dt);
        bool wrote = apply_result || layered;
        if (wrote)
        {
            if (!guarded_write(desired_view, &view, VIEW_BYTES))
            {
                lose_view();
                return;
            }
        }
        g_view.out_rotation = dw::multiply(result_rotation, dw::conjugate(rotation));
        g_view.out_offset = pivot + dw::rotate(g_view.out_rotation, camera - pivot) - result;
        // Owned: the game's FOV, not a layer's, so the glide back starts from the view the owner left on screen.
        g_view.out_fov = fov_ok ? (owned ? game_view.fov : view.fov) : NAN;
        g_view.out_valid = true;
        lua::g_blending.store(g_view.blending, std::memory_order_relaxed);
        g_debug_glide.store(g_view.blending && g_view.blend_glide, std::memory_order_relaxed);
        g_debug_lag_h.store(std::hypot(g_view.out_offset.x, g_view.out_offset.y), std::memory_order_relaxed);
        g_debug_lag_v.store(std::abs(g_view.out_offset.z), std::memory_order_relaxed);
        publish_api_view(game_view, wrote ? view : game_view, &pivot);
    }

    struct InHook
    {
        InHook() { g_in_hook.fetch_add(1, std::memory_order_relaxed); }
        ~InHook() { g_in_hook.fetch_sub(1, std::memory_order_relaxed); }
    };

    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view)
    {
        InHook in_hook;
        g_original(self, delta_time, desired_view);
        if (self != g_player_camera.load(std::memory_order_relaxed)) return;
        g_view_updates.fetch_add(1, std::memory_order_relaxed);
        // One writer: the player's camera updates in sequence.
        if (std::isfinite(delta_time) && delta_time > 0.0f)
        {
            g_view_seconds.store(g_view_seconds.load(std::memory_order_relaxed) + delta_time, std::memory_order_relaxed);
        }
        LARGE_INTEGER stamp{};
        QueryPerformanceCounter(&stamp);
        g_last_view_qpc.store(stamp.QuadPart, std::memory_order_relaxed);
        // The processor slot and its switch, sampled once for this whole update (Processor::state). No processor
        // reads as off with toggle generation 0: the core alone leaves the game's view alone.
        Counted in_processor(g_in_processor);
        const Processor* processor = g_processor.load();
        uint64_t toggle = 0;
        const bool enabled = processor && processor->state(processor->user, &toggle) != 0;
        // Off and settled: the game's view untouched. The next toggle starts from a fresh output.
        if (!enabled && !g_view.blending && toggle == g_view.seen_toggle && !lua::g_layers_any.load(std::memory_order_relaxed))
        {
            g_view.valid = false;
            g_view.invalid_reason = dw::Snap::Toggle;
            g_view.out_valid = false;
            lua::g_blending.store(false, std::memory_order_relaxed);
            publish_debug_idle();
            ViewHead view{};
            if (guarded_read(desired_view, &view, VIEW_BYTES)) publish_api_view(view, view, nullptr);
            return;
        }
        if (!g_hook_timing.load(std::memory_order_relaxed))
        {
            update_view(desired_view, delta_time, processor, enabled, toggle);
            return;
        }
        LARGE_INTEGER start{}, stop{};
        QueryPerformanceCounter(&start);
        update_view(desired_view, delta_time, processor, enabled, toggle);
        QueryPerformanceCounter(&stop);
        g_calls_timed.fetch_add(1, std::memory_order_relaxed);
        g_ticks_spent.fetch_add(static_cast<uint64_t>(stop.QuadPart - start.QuadPart), std::memory_order_relaxed);
    }

    // False under a pause, a load, a cutscene or the free camera: the player's camera is not updating.
    auto camera_live() -> bool
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        auto last = g_last_view_qpc.load(std::memory_order_relaxed);
        return last != 0 && static_cast<double>(now.QuadPart - last) / static_cast<double>(g_qpc_frequency.QuadPart) < 0.25;
    }

    // Lua's Smoothwalker.enabled() (lua::g_enabled): the processor's switch, false without one.
    auto processor_enabled() -> bool
    {
        Counted in_processor(g_in_processor);
        const Processor* processor = g_processor.load();
        uint64_t toggle = 0;
        return processor && processor->state(processor->user, &toggle) != 0;
    }

    // Game thread: a listener callback, if one is set and has it.
    template <typename Call>
    auto notify(Call&& call) -> void
    {
        Counted in_listener(g_in_listener);
        if (const Listener* listener = g_listener.load()) call(*listener);
    }

    // Unregistering: after the slot is cleared, until no call counted before the clear is still running.
    // False: a counted call is still running after 5 s (the table's 0).
    auto wait_for_zero(const std::atomic<int>& count) -> bool
    {
        for (int i = 0; i < 5000 && count.load() != 0; ++i) Sleep(1);
        return count.load() == 0;
    }

    // ---------------------------------------------------------------------------------------------- the table

    auto api_register_processor(const Processor* p) -> int32_t
    {
        if (!p || p->size < sizeof(Processor) || !p->frame || !p->state) return 0;
        const Processor* none = nullptr;
        return g_processor.compare_exchange_strong(none, p) ? 1 : 0;
    }

    auto api_unregister_processor(const Processor* p) -> int32_t
    {
        const Processor* held = p;
        if (!p || !g_processor.compare_exchange_strong(held, nullptr)) return 1; // not registered: nothing can call p
        return wait_for_zero(g_in_processor) ? 1 : 0;
    }

    auto api_set_listener(const Listener* l) -> int32_t
    {
        if (!l || l->size < sizeof(Listener)) return 0;
        const Listener* none = nullptr;
        return g_listener.compare_exchange_strong(none, l) ? 1 : 0;
    }

    auto api_clear_listener(const Listener* l) -> int32_t
    {
        const Listener* held = l;
        if (!l || !g_listener.compare_exchange_strong(held, nullptr)) return 1; // not set: nothing can call l
        return wait_for_zero(g_in_listener) ? 1 : 0;
    }

    auto api_set_cut_thresholds(double reset_distance, double reset_gap) -> void
    {
        if (!std::isfinite(reset_distance) || !std::isfinite(reset_gap)) return;
        AcquireSRWLockExclusive(&g_tuning_lock);
        // No g_reset: the hook crossfades on the new generation instead of snapping the lag away mid-motion. Only a
        // changed value starts one.
        if (g_tuning.reset_distance != reset_distance || g_tuning.reset_gap != reset_gap)
        {
            g_tuning = Tuning{reset_distance, reset_gap, g_tuning.generation + 1};
        }
        ReleaseSRWLockExclusive(&g_tuning_lock);
    }

    auto api_set_diagnostics(uint32_t flags) -> void
    {
        g_log_verbose.store((flags & DIAG_VERBOSE) != 0);
        g_hook_timing.store((flags & DIAG_HOOK_TIMING) != 0);
    }

    auto api_player_camera() -> void* { return g_player_camera.load(std::memory_order_relaxed); }
    auto api_player_controller() -> void* { return g_player_controller.load(std::memory_order_relaxed); }
    auto api_player_known() -> int32_t { return g_player_known.load() ? 1 : 0; }
    auto api_view_updates() -> uint64_t { return g_view_updates.load(); }
    auto api_view_seconds() -> double { return g_view_seconds.load(); }
    auto api_camera_live() -> int32_t { return camera_live() ? 1 : 0; }
    auto api_game_thread_id() -> uint32_t { return lua::g_game_thread.load(std::memory_order_relaxed); }

    auto api_take_hook_timing(uint64_t* calls, double* microseconds_per_call) -> void
    {
        auto timed = g_calls_timed.exchange(0);
        auto ticks = g_ticks_spent.exchange(0);
        if (calls) *calls = timed;
        if (microseconds_per_call)
        {
            *microseconds_per_call = timed ? 1e6 * static_cast<double>(ticks) / static_cast<double>(g_qpc_frequency.QuadPart) / timed : 0.0;
        }
    }

    auto api_read_debug(Debug* out) -> void
    {
        if (!out || out->size < sizeof(Debug)) return;
        out->keep_follow = g_debug_keep_follow.load(std::memory_order_relaxed);
        out->keep_turn = g_debug_keep_turn.load(std::memory_order_relaxed);
        out->rate_h = g_debug_rate_h.load(std::memory_order_relaxed);
        out->influence = g_debug_influence.load(std::memory_order_relaxed);
        out->lag_h = g_debug_lag_h.load(std::memory_order_relaxed);
        out->lag_v = g_debug_lag_v.load(std::memory_order_relaxed);
        out->snap = g_debug_snap.load(std::memory_order_relaxed);
        out->snap_age = NAN;
        if (auto at = g_debug_snap_qpc.load(std::memory_order_relaxed))
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            out->snap_age = static_cast<double>(now.QuadPart - at) / static_cast<double>(g_qpc_frequency.QuadPart);
        }
        out->glide = g_debug_glide.load(std::memory_order_relaxed) ? 1 : 0;
    }

    // Read-only, the way Smoothwalker.owner() reads it: under lua::g_mutex, and an expired lease reads as nobody.
    // The drop itself stays with claim and release.
    auto api_camera_owner(char* name, uint32_t capacity, double* lease_seconds) -> uint32_t
    {
        std::string mod;
        int64_t expires = 0;
        {
            std::lock_guard guard(lua::g_mutex);
            expires = lua::g_owner_expires.load(std::memory_order_relaxed);
            if (lua::g_owner_state && (expires == 0 || lua::qpc_now() < expires)) mod = lua::g_owner_mod;
        }
        if (lease_seconds)
        {
            *lease_seconds = !mod.empty() && expires != 0 ? std::max(0.0, static_cast<double>(expires - lua::qpc_now()) / lua::qpc_frequency()) : NAN;
        }
        if (name && capacity > 0)
        {
            size_t n = std::min<size_t>(mod.size(), capacity - 1);
            memcpy(name, mod.data(), n);
            name[n] = '\0';
        }
        return static_cast<uint32_t>(mod.size());
    }

    const Api g_api{
            sizeof(Api),
            API_VERSION,
            &api_register_processor,
            &api_unregister_processor,
            &api_set_listener,
            &api_clear_listener,
            &api_set_cut_thresholds,
            &api_set_diagnostics,
            &api_player_camera,
            &api_player_controller,
            &api_player_known,
            &api_view_updates,
            &api_view_seconds,
            &api_camera_live,
            &api_game_thread_id,
            &api_take_hook_timing,
            &api_read_debug,
            &api_camera_owner,
    };

    auto get_api(uint32_t version) -> const Api*
    {
        return version >= 1 && version <= API_VERSION ? &g_api : nullptr;
    }
} // namespace
} // namespace dw::camera
