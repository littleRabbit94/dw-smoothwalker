// The camera core's hook side (camera/pipeline.hpp): one player-camera update, and the CameraCore state around it.
// The QueryPerformanceCounter calls are part of the contract: the equivalence harness counts them, in order.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "pipeline.hpp"
#include "authority.hpp"
#include "hook.hpp"
#include "wall.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace dw::camera
{
namespace
{
    auto finite(const dw::Vec3& v) -> bool
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // Unregistering: after the slot is cleared, until no call counted before the clear is still running.
    // False: a counted call is still running after 5 s.
    auto wait_for_zero(const std::atomic<int>& count) -> bool
    {
        for (int i = 0; i < 5000 && count.load() != 0; ++i) Sleep(1);
        return count.load() == 0;
    }
} // namespace

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

    auto Pipeline::seconds_between(LARGE_INTEGER a, LARGE_INTEGER b) const -> double
    {
        return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(m_qpc_frequency.QuadPart);
    }

    auto Pipeline::publish_api_view(const ViewHead& game, const ViewHead& shown, const dw::Vec3* pivot) -> void
    {
        View g{{game.location[0], game.location[1], game.location[2]}, {game.rotation[0], game.rotation[1], game.rotation[2]}, game.fov};
        View s{{shown.location[0], shown.location[1], shown.location[2]}, {shown.rotation[0], shown.rotation[1], shown.rotation[2]}, shown.fov};
        double p[3]{};
        if (pivot) { p[0] = pivot->x; p[1] = pivot->y; p[2] = pivot->z; }
        m_snapshot.publish(g, s, pivot ? p : nullptr);
    }

    // The debug overlay's feed while nothing is followed.
    auto Pipeline::publish_debug_idle() -> void
    {
        m_debug_keep_follow.store(NAN, std::memory_order_relaxed);
        m_debug_keep_turn.store(NAN, std::memory_order_relaxed);
        m_debug_lag_h.store(NAN, std::memory_order_relaxed);
        m_debug_lag_v.store(NAN, std::memory_order_relaxed);
        m_debug_rate_h.store(NAN, std::memory_order_relaxed);
        m_debug_glide.store(false, std::memory_order_relaxed);
    }

    auto Pipeline::lose_view() -> void
    {
        m_view.valid = false;
        m_view.invalid_reason = Snap::ViewLost;
        m_view.out_valid = false;
        m_view.blending = false;
        g_authority.set_blending(false);
        publish_debug_idle();
    }

    // The core's pipeline, once per player-camera update: read the game's view and the pivot, work out cuts and
    // ownership, let the processor move the camera, crossfade any change, apply other mods' layers, write the view
    // back and publish it (docs/design.md, "Core and processors"). processor, enabled and toggle: the hook's one
    // sample of the processor slot and its switch for this update.
    auto Pipeline::update_view(void* desired_view, float delta_time, Processor* processor, bool enabled, uint64_t toggle) -> void
    {
        // Ownership is sampled once, first, and that one sample is used for the whole update. A release runs on the
        // game thread while this runs on a worker: sampling the owner slot after the release generation and m_reset
        // could see the release already published and the camera still owned, or the other way round, and write a
        // full follow offset for one frame, which turns a glide into a cut and a cut into a double snap. A lease
        // that has run out is not a claim; the game thread drops the identity the next time it looks (Authority).
        // The slot is read before the lease, so a fresh claim never pairs with the previous owner's stale expiry.
        const bool owner_held = g_authority.owner_slot() >= 0;
        const int64_t owner_expires = g_authority.owner_expires();
        const bool owned = owner_held && (owner_expires == 0 || qpc_now() < owner_expires);
        const bool owner_keeps_layers = owned && g_authority.owner_keeps_layers();
        // The release generation, sampled here because the falling edge below needs it; `changed` uses this sample.
        const auto release = g_authority.release_generation();
        const bool release_changed = release != m_view.seen_release;
        // The camera stops being owned. A release("cut") has already set m_reset, and a release("glide") has bumped
        // the generation just read, which starts the crossfade from out_*, the game's view as the owner left it. A
        // lease running out, an uninstall or an install re-key say nothing, and writing the follow offset that piled
        // up while owned would pop the camera in that one frame and pop it again when the game thread later notices
        // and sets m_reset: those edges restart the follow from the capsule here, which is what a cut does. A glide
        // is left alone, because resetting would throw away the warm follow it is meant to ease back into and the
        // crossfade would run from nothing to nothing.
        if (m_view.was_owned && !owned && !release_changed)
        {
            m_view.valid = false;
            m_view.invalid_reason = Snap::ClaimEnded;
        }
        m_view.was_owned = owned;

        AcquireSRWLockShared(&m_tuning_lock);
        const Tuning t = m_tuning;
        ReleaseSRWLockShared(&m_tuning_lock);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        auto* root = m_player_root.load(std::memory_order_relaxed);
        auto offset = m_translation_offset.load(std::memory_order_relaxed);
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
        auto half_offset = m_half_height_offset.load(std::memory_order_relaxed);
        if (half_offset >= 0 && !guarded_read(static_cast<uint8_t*>(root) + half_offset, &half_height, sizeof(half_height))) half_height = NAN;
        if (!std::isfinite(half_height) || half_height < 0.0f || half_height > 1000.0f) half_height = NAN;

        // The cut thresholds, the processor's switch and settings, and its mode writes crossfade; a hard cut (player
        // or world change, a gap, a teleport) snaps. A release("glide") lands here too, through the `release_changed`
        // sampled at the top of this update: the fade then starts from the game's view, because out_* tracked it
        // while the camera was owned (camera/authority.hpp). The processor's generation joins below, with the frame it
        // reports it for.
        bool changed = t.generation != m_view.seen_tuning || toggle != m_view.seen_toggle || release_changed;
        m_view.seen_tuning = t.generation;
        m_view.seen_toggle = toggle;
        m_view.seen_release = release;
        const bool reset = m_reset.exchange(false, std::memory_order_relaxed);
        const bool gap = seconds_between(m_view.last_call, now) > t.reset_gap;
        const bool jump = !(dw::length(pivot - m_view.pivot_last) <= t.reset_distance);
        bool cut = reset || gap || jump;
        m_view.last_call = now;
        m_view.pivot_last = pivot;

        // The world delta, so slow motion slows the follow and the crossfade with the game. A pause is not
        // held: the camera stops updating under one, and the first update after it is a cut (reset_gap).
        double dt = std::clamp(static_cast<double>(delta_time), 0.0, 0.1);

        bool restart = false;
        if (!enabled)
        {
            m_view.valid = false; // switched back on, the follow restarts from the capsule
            m_view.invalid_reason = Snap::Toggle;
            m_debug_keep_follow.store(NAN, std::memory_order_relaxed);
            m_debug_keep_turn.store(NAN, std::memory_order_relaxed);
            m_debug_rate_h.store(NAN, std::memory_order_relaxed);
        }
        else if (cut || !m_view.valid)
        {
            // Why, for the debug overlay: switched back on beats a cut still pending from while it was off; then
            // whoever asked for the cut; then why the follow was dropped; then the hook's own gap and teleport checks.
            auto why = !m_view.valid && m_view.invalid_reason == Snap::Toggle ? Snap::Toggle
                       : reset                                                ? static_cast<Snap>(m_reset_reason.load(std::memory_order_relaxed))
                       : !m_view.valid                                        ? m_view.invalid_reason
                       : gap                                                  ? Snap::Gap
                                                                              : Snap::Teleport;
            m_debug_snap_qpc.store(now.QuadPart, std::memory_order_relaxed);
            m_debug_snap.store(static_cast<int>(why), std::memory_order_relaxed);
            m_view.valid = true;
            restart = true;
        }

        FrameIn in{};
        in.enabled = enabled;
        in.restart = restart;
        in.dt = dt;
        in.pivot = pivot;
        in.half_height = static_cast<double>(half_height);
        in.camera = camera;
        in.rotation = rotation;
        FrameOut moved{};
        if (processor) processor->frame(in, moved);
        if (moved.generation != m_view.seen_processor) changed = true;
        m_view.seen_processor = moved.generation;

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
            m_debug_keep_follow.store(moved.keep_follow, std::memory_order_relaxed);
            m_debug_keep_turn.store(moved.keep_turn, std::memory_order_relaxed);
            m_debug_rate_h.store(moved.rate_h, std::memory_order_relaxed);
            m_debug_influence.store(moved.influence, std::memory_order_relaxed);
        }

        // Another mod owns the camera (camera/authority.hpp; `owned` was sampled at the top of this update).
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
            m_view.blending = false;
        }
        else if (transition <= 0.0)
        {
            m_view.blending = false; // set to 0 mid-fade: the user's choice is no fade
        }
        else if (changed && m_view.out_valid)
        {
            m_view.blending = true;
            m_view.blend_glide = release_changed;
            m_view.blend_elapsed = 0.0;
            m_view.blend_duration = transition;
            m_view.from_offset = m_view.out_offset;
            m_view.from_rotation = m_view.out_rotation;
            m_view.from_fov = m_view.out_fov;
        }
        bool blended = m_view.blending;
        if (blended)
        {
            m_view.blend_elapsed += dt;
            double s = std::min(m_view.blend_elapsed / m_view.blend_duration, 1.0);
            double w = s * s * (3.0 - 2.0 * s);
            dw::Vec3 game_arm = camera - pivot;
            dw::Quat target = dw::multiply(result_rotation, dw::conjugate(rotation));
            dw::Quat d = dw::slerp(m_view.from_rotation, target, w);
            dw::Vec3 lag = pivot + dw::rotate(target, game_arm) - result;
            result = pivot - (m_view.from_offset + (lag - m_view.from_offset) * w) + dw::rotate(d, game_arm);
            result_rotation = dw::multiply(d, rotation);
            dw::to_rotator(result_rotation, view.rotation[0], view.rotation[1], view.rotation[2]);
            if (fov_ok && std::isfinite(m_view.from_fov)) view.fov = m_view.from_fov + static_cast<float>((view.fov - m_view.from_fov) * w);
            if (s >= 1.0) m_view.blending = false;

            // The faded part of the lag was never clamped: keep it in front of a wall the game pulled in for.
            double game_distance = dw::length(game_arm);
            // The toggle-off fade too: it ends at the game's view, so clamping to the game's distance never moves the endpoint.
            if ((!enabled || m_view.valid) && moved.wall_clamp)
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
        // Other mods' layers (Authority::apply_layers), on top of whatever the processor did, its switch included.
        // While another mod owns the camera they are off too, unless that owner asked to keep them.
        bool layered = (!owned || owner_keeps_layers) && g_authority.apply_layers(view.location, view.rotation, view.fov, dt);
        bool wrote = apply_result || layered;
        if (wrote)
        {
            if (!guarded_write(desired_view, &view, VIEW_BYTES))
            {
                lose_view();
                return;
            }
        }
        m_view.out_rotation = dw::multiply(result_rotation, dw::conjugate(rotation));
        m_view.out_offset = pivot + dw::rotate(m_view.out_rotation, camera - pivot) - result;
        // Owned: the game's FOV, not a layer's, so the glide back starts from the view the owner left on screen.
        m_view.out_fov = fov_ok ? (owned ? game_view.fov : view.fov) : NAN;
        m_view.out_valid = true;
        g_authority.set_blending(m_view.blending);
        m_debug_glide.store(m_view.blending && m_view.blend_glide, std::memory_order_relaxed);
        m_debug_lag_h.store(std::hypot(m_view.out_offset.x, m_view.out_offset.y), std::memory_order_relaxed);
        m_debug_lag_v.store(std::abs(m_view.out_offset.z), std::memory_order_relaxed);
        publish_api_view(game_view, wrote ? view : game_view, &pivot);
    }

    auto Pipeline::on_camera_view(void* self, float delta_time, void* desired_view) -> void
    {
        if (self != m_player_camera.load(std::memory_order_relaxed)) return;
        m_view_updates.fetch_add(1, std::memory_order_relaxed);
        // One writer: the player's camera updates in sequence.
        if (std::isfinite(delta_time) && delta_time > 0.0f)
        {
            m_view_seconds.store(m_view_seconds.load(std::memory_order_relaxed) + delta_time, std::memory_order_relaxed);
        }
        LARGE_INTEGER stamp{};
        QueryPerformanceCounter(&stamp);
        m_last_view_qpc.store(stamp.QuadPart, std::memory_order_relaxed);
        // The processor slot and its switch, sampled once for this whole update (Processor::state). No processor
        // reads as off with toggle generation 0: the core alone leaves the game's view alone.
        Counted in_processor(m_in_processor);
        Processor* processor = m_processor.load();
        uint64_t toggle = 0;
        const bool enabled = processor && processor->state(toggle);
        // Off and settled: the game's view untouched. The next toggle starts from a fresh output.
        if (!enabled && !m_view.blending && toggle == m_view.seen_toggle && !g_authority.layers_any())
        {
            m_view.valid = false;
            m_view.invalid_reason = Snap::Toggle;
            m_view.out_valid = false;
            g_authority.set_blending(false);
            publish_debug_idle();
            ViewHead view{};
            if (guarded_read(desired_view, &view, VIEW_BYTES)) publish_api_view(view, view, nullptr);
            return;
        }
        if (!m_hook_timing.load(std::memory_order_relaxed))
        {
            update_view(desired_view, delta_time, processor, enabled, toggle);
            return;
        }
        LARGE_INTEGER start{}, stop{};
        QueryPerformanceCounter(&start);
        update_view(desired_view, delta_time, processor, enabled, toggle);
        QueryPerformanceCounter(&stop);
        m_calls_timed.fetch_add(1, std::memory_order_relaxed);
        m_ticks_spent.fetch_add(static_cast<uint64_t>(stop.QuadPart - start.QuadPart), std::memory_order_relaxed);
    }

    auto Pipeline::camera_live() const -> bool
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        auto last = m_last_view_qpc.load(std::memory_order_relaxed);
        return last != 0 && static_cast<double>(now.QuadPart - last) / static_cast<double>(m_qpc_frequency.QuadPart) < 0.25;
    }

    auto Pipeline::processor_enabled() -> bool
    {
        Counted in_processor(m_in_processor);
        Processor* processor = m_processor.load();
        uint64_t toggle = 0;
        return processor && processor->state(toggle);
    }

    auto Pipeline::register_processor(Processor& p) -> bool
    {
        Processor* none = nullptr;
        return m_processor.compare_exchange_strong(none, &p);
    }

    auto Pipeline::unregister_processor(Processor& p) -> bool
    {
        Processor* held = &p;
        if (!m_processor.compare_exchange_strong(held, nullptr)) return true; // not registered: nothing can call p
        return wait_for_zero(m_in_processor);
    }

    auto Pipeline::set_listener(Listener& l) -> bool
    {
        Listener* none = nullptr;
        return m_listener.compare_exchange_strong(none, &l);
    }

    auto Pipeline::clear_listener(Listener& l) -> bool
    {
        Listener* held = &l;
        if (!m_listener.compare_exchange_strong(held, nullptr)) return true; // not set: nothing can call l
        return wait_for_zero(m_in_listener);
    }

    auto Pipeline::set_cut_thresholds(double reset_distance, double reset_gap) -> void
    {
        if (!std::isfinite(reset_distance) || !std::isfinite(reset_gap)) return;
        AcquireSRWLockExclusive(&m_tuning_lock);
        // No m_reset: the hook crossfades on the new generation instead of snapping the lag away mid-motion. Only a
        // changed value starts one.
        if (m_tuning.reset_distance != reset_distance || m_tuning.reset_gap != reset_gap)
        {
            m_tuning = Tuning{reset_distance, reset_gap, m_tuning.generation + 1};
        }
        ReleaseSRWLockExclusive(&m_tuning_lock);
    }

    auto Pipeline::set_diagnostics(uint32_t flags) -> void
    {
        m_log_verbose.store((flags & DIAG_VERBOSE) != 0);
        m_hook_timing.store((flags & DIAG_HOOK_TIMING) != 0);
    }

    auto Pipeline::take_hook_timing(uint64_t& calls, double& microseconds_per_call) -> void
    {
        auto timed = m_calls_timed.exchange(0);
        auto ticks = m_ticks_spent.exchange(0);
        calls = timed;
        microseconds_per_call = timed ? 1e6 * static_cast<double>(ticks) / static_cast<double>(m_qpc_frequency.QuadPart) / timed : 0.0;
    }

    auto Pipeline::read_debug() const -> DebugFeed
    {
        DebugFeed out{};
        out.keep_follow = m_debug_keep_follow.load(std::memory_order_relaxed);
        out.keep_turn = m_debug_keep_turn.load(std::memory_order_relaxed);
        out.rate_h = m_debug_rate_h.load(std::memory_order_relaxed);
        out.influence = m_debug_influence.load(std::memory_order_relaxed);
        out.lag_h = m_debug_lag_h.load(std::memory_order_relaxed);
        out.lag_v = m_debug_lag_v.load(std::memory_order_relaxed);
        out.snap = m_debug_snap.load(std::memory_order_relaxed);
        out.snap_age = NAN;
        if (auto at = m_debug_snap_qpc.load(std::memory_order_relaxed))
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            out.snap_age = static_cast<double>(now.QuadPart - at) / static_cast<double>(m_qpc_frequency.QuadPart);
        }
        out.glide = m_debug_glide.load(std::memory_order_relaxed);
        return out;
    }

    auto Pipeline::reset() -> void
    {
        AcquireSRWLockExclusive(&m_tuning_lock);
        m_tuning = DEFAULT_TUNING;
        ReleaseSRWLockExclusive(&m_tuning_lock);
        m_reset.store(true);
        m_reset_reason.store(static_cast<int>(Snap::Startup));
        m_hook_timing.store(false);
        m_log_verbose.store(false);
        m_player_camera.store(nullptr);
        m_player_root.store(nullptr);
        m_player_controller.store(nullptr);
        m_player_known.store(false);
        m_translation_offset.store(-1);
        m_half_height_offset.store(-1);
        m_view_updates.store(0);
        m_last_view_qpc.store(0);
        m_view_seconds.store(0.0);
        m_calls_timed.store(0);
        m_ticks_spent.store(0);
        m_debug_keep_follow.store(NAN);
        m_debug_keep_turn.store(NAN);
        m_debug_influence.store(0);
        m_debug_lag_h.store(NAN);
        m_debug_lag_v.store(NAN);
        m_debug_rate_h.store(NAN);
        m_debug_snap.store(0);
        m_debug_snap_qpc.store(0);
        m_debug_glide.store(false);
        m_view = ViewState{};
        m_processor.store(nullptr);
        m_listener.store(nullptr);
        m_snapshot.reset();
    }

    auto Pipeline::inspect() const -> Inspect
    {
        Inspect out{};
        out.keep_follow = m_debug_keep_follow.load();
        out.keep_turn = m_debug_keep_turn.load();
        out.influence = m_debug_influence.load();
        out.lag_h = m_debug_lag_h.load();
        out.lag_v = m_debug_lag_v.load();
        out.rate_h = m_debug_rate_h.load();
        out.snap = m_debug_snap.load();
        out.snap_qpc = m_debug_snap_qpc.load();
        out.glide = m_debug_glide.load();
        out.view_updates = m_view_updates.load();
        out.view_seconds = m_view_seconds.load();
        out.last_view_qpc = m_last_view_qpc.load();
        out.reset = m_reset.load();
        out.reset_reason = m_reset_reason.load();
        return out;
    }

    // ------------------------------------------------------------------------------------------ the interface

namespace
{
    // The core's CameraCore (camera/api.hpp): methods over the static Pipeline (camera/hook.cpp) and the Authority, no
    // state of its own.
    class CoreApi final : public CameraCore
    {
      public:
        auto register_processor(Processor& p) -> bool override { return g_pipeline.register_processor(p); }
        auto unregister_processor(Processor& p) -> bool override { return g_pipeline.unregister_processor(p); }
        auto set_listener(Listener& l) -> bool override { return g_pipeline.set_listener(l); }
        auto clear_listener(Listener& l) -> bool override { return g_pipeline.clear_listener(l); }
        auto set_cut_thresholds(double reset_distance, double reset_gap) -> void override { g_pipeline.set_cut_thresholds(reset_distance, reset_gap); }
        auto set_diagnostics(uint32_t flags) -> void override { g_pipeline.set_diagnostics(flags); }

        // The hook compares the player camera with its untyped `self`, so the Pipeline's slots stay void*; the type is
        // put back here.
        auto player_camera() const -> RC::Unreal::UObject* override { return static_cast<RC::Unreal::UObject*>(g_pipeline.player_camera()); }
        auto player_controller() const -> RC::Unreal::UObject* override
        {
            return static_cast<RC::Unreal::UObject*>(g_pipeline.player_controller());
        }
        auto player_known() const -> bool override { return g_pipeline.player_known(); }
        auto view_updates() const -> uint64_t override { return g_pipeline.view_updates(); }
        auto view_seconds() const -> double override { return g_pipeline.view_seconds(); }
        auto camera_live() const -> bool override { return g_pipeline.camera_live(); }
        auto game_thread_id() const -> uint32_t override { return g_authority.game_thread(); }
        auto take_hook_timing(uint64_t& calls, double& microseconds_per_call) -> void override { g_pipeline.take_hook_timing(calls, microseconds_per_call); }
        auto read_debug() const -> DebugFeed override { return g_pipeline.read_debug(); }

        // Read-only, the way Smoothwalker.owner() reads it (Authority::owner): under the Authority's mutex, and an
        // expired lease reads as nobody. The drop itself stays with claim and release.
        auto camera_owner() const -> Owner override
        {
            int64_t expires = 0;
            std::string mod = g_authority.owner(&expires);
            Owner owner;
            owner.lease = !mod.empty() && expires != 0 ? std::max(0.0, static_cast<double>(expires - qpc_now()) / qpc_frequency()) : NAN;
            owner.mod = std::move(mod);
            return owner;
        }
    };
    CoreApi g_core_api; // stateless, trivially destructible: nothing to reset, nothing run at unload
} // namespace

    auto core_api() -> CameraCore&
    {
        return g_core_api;
    }

    auto processor_enabled() -> bool
    {
        return g_pipeline.processor_enabled();
    }
} // namespace dw::camera
