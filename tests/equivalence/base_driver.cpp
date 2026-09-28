// The baseline build: commit 47f6645's headers and the hook region of its dllmain.cpp (from `namespace` to the end
// of camera_live), extracted by run.sh into base_region.inc and compiled unchanged. The mod-object code that fed the
// hook at 47f6645 (publish_locked's hook part, set_enabled_locked, apply_position's flags and write, on_update's
// report, debug_panel's reads) is quoted below where the region cannot supply it. Namespaces are renamed base_* on
// the command line so both builds link into one program.

#include "prelude.hpp" // first: see there

#include "config.hpp"
#include "core/frame.hpp"
#include "core/wall.hpp"
#include "follow/follow.hpp"
#include "smoothing.hpp"
#include "lua_api.hpp"
#undef ifstream

#include "harness.hpp"

#include "base_region.inc"

namespace
{
#include "api_ops.inc"

    class BaseDriver final : public harness::Variant
    {
      public:
        auto init(const harness::HSettings& settings, bool enabled) -> void override
        {
            // The constructor at 47f6645.
            dwapi::g_enabled = &g_enabled;
            dwapi::g_reset = &g_reset;
            dwapi::g_reset_reason = &g_reset_reason;
            QueryPerformanceFrequency(&g_qpc_frequency);
            g_original = &harness::original_view;
            ops::install(0);
            ops::install(1);
            publish(settings);
            g_enabled.store(enabled); // reload_settings_locked(true), after its publish_locked()
        }

        auto publish(const harness::HSettings& h) -> void override
        {
            auto settings = harness::to_settings<dwsc::Settings>(h);
            // publish_locked at 47f6645, up to the hook's last setting.
            auto tuning = tuning_of(settings);
            auto follow = follow_tuning_of(settings);
            AcquireSRWLockExclusive(&g_tuning_lock);
            if (!same_values(tuning, g_tuning))
            {
                tuning.generation = ++m_tuning_generation;
                g_tuning = tuning;
            }
            if (!dwsw::same_values(follow, g_follow_tuning))
            {
                g_follow_tuning = follow;
                ++g_follow_generation;
            }
            ReleaseSRWLockExclusive(&g_tuning_lock);
            g_log_stats.store(settings.log_stats);
            dwsc::g_log_verbose.store(settings.log_verbose);
        }

        auto set_enabled(bool on) -> void override
        {
            // set_enabled_locked at 47f6645.
            if (g_enabled.load() == on) return;
            g_toggle_generation.fetch_add(1);
            g_enabled.store(on);
        }

        auto set_modes(bool aiming, bool combat, bool traversal, harness::Record& out) -> void override
        {
            // apply_position at 47f6645.
            g_aiming.store(aiming);
            out.put("modes.combat_was", g_combat.exchange(combat));
            out.put("modes.traversal_was", g_traversal.exchange(traversal));
        }

        auto mode_write() -> void override { g_position_generation.fetch_add(1); }

        auto request_cut(int reason) -> void override { ::request_cut(static_cast<dwsc::Snap>(reason)); }

        auto set_player(void* camera, void* root, int32_t translation_offset, int32_t half_height_offset) -> void override
        {
            g_player_camera.store(camera);
            g_player_root.store(root);
            g_translation_offset.store(translation_offset);
            g_half_height_offset.store(half_height_offset);
        }

        auto hook(void* self, float delta_time, void* desired_view) -> void override { get_camera_view_hook(self, delta_time, desired_view); }

        auto claim(int consumer, bool keep_layers, double ttl) -> int override { return ops::claim(consumer, keep_layers, ttl); }
        auto release(int consumer, bool glide) -> int override { return ops::release(consumer, glide); }
        auto uninstall(int consumer) -> void override { ops::uninstall(consumer); }
        auto install(int consumer) -> void override { ops::install(consumer); }
        auto layer_set(int consumer, const harness::LayerArgs& args) -> int override { return ops::layer_set(consumer, args); }
        auto layer_clear(int consumer) -> int override { return ops::layer_clear(consumer); }

        auto take_stats(harness::Record& r) -> void override
        {
            // on_update's report at 47f6645, over a fixed 5 s.
            r.put("stats.log_stats", g_log_stats.load());
            auto frames = g_frames.exchange(0);
            auto clamped = g_clamped.exchange(0);
            auto lag = g_lag_sum.exchange(0.0);
            auto timed = g_calls_timed.exchange(0);
            auto ticks = g_ticks_spent.exchange(0);
            double micros = timed ? 1e6 * static_cast<double>(ticks) / static_cast<double>(g_qpc_frequency.QuadPart) / timed : 0.0;
            const double elapsed = 5.0;
            r.put("stats.frames", frames);
            r.put("stats.clamped", clamped);
            r.put("stats.lag", lag);
            r.put("stats.timed", timed);
            r.put("stats.micros", micros);
            r.put("stats.rate", frames / elapsed);
            r.put("stats.mean_lag", frames ? lag / frames : 0.0);
            r.put("stats.clamp_share", frames ? 100.0 * clamped / frames : 0.0);
        }

        auto panel(harness::Record& r) -> void override
        {
            // debug_panel at 47f6645 (the parts off UObjects), in its order, then the keys' reads.
            r.put("panel.enabled", g_enabled.load());
            AcquireSRWLockShared(&g_tuning_lock);
            r.put("panel.max_lag_h", g_follow_tuning.max_lag_h);
            r.put("panel.max_lag_v", g_follow_tuning.max_lag_v);
            r.put("panel.rotation_smoothing", g_follow_tuning.rotation_smoothing);
            ReleaseSRWLockShared(&g_tuning_lock);
            r.put("panel.keep_follow", g_debug_keep_follow.load(std::memory_order_relaxed));
            r.put("panel.keep_turn", g_debug_keep_turn.load(std::memory_order_relaxed));
            r.put("panel.influence", g_debug_influence.load(std::memory_order_relaxed));
            r.put("panel.lag_h", g_debug_lag_h.load(std::memory_order_relaxed));
            r.put("panel.lag_v", g_debug_lag_v.load(std::memory_order_relaxed));
            r.put("panel.rate_h", g_debug_rate_h.load(std::memory_order_relaxed));
            r.put("panel.snap", g_debug_snap.load(std::memory_order_relaxed));
            double snap_age = NAN;
            if (auto at = g_debug_snap_qpc.load(std::memory_order_relaxed))
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                snap_age = static_cast<double>(now.QuadPart - at) / static_cast<double>(g_qpc_frequency.QuadPart);
            }
            r.put("panel.snap_age", snap_age);
            // dwsc::api_status at 47f6645 (debug_overlay.hpp).
            std::string mod;
            int64_t expires = 0;
            {
                std::lock_guard guard(dwapi::g_mutex);
                expires = dwapi::g_owner_expires.load(std::memory_order_relaxed);
                if (dwapi::g_owner_state && (expires == 0 || dwapi::qpc_now() < expires)) mod = dwapi::g_owner_mod;
            }
            double lease = NAN;
            if (!mod.empty() && expires != 0) lease = std::max(0.0, static_cast<double>(expires - dwapi::qpc_now()) / dwapi::qpc_frequency());
            r.put("panel.owner", mod.data(), mod.size());
            r.put("panel.lease", lease);
            r.put("panel.glide", g_debug_glide.load(std::memory_order_relaxed));
            // key_live, the flush gate and apply_position.
            r.put("keys.camera_live", camera_live());
            r.put("tuner.view_updates", g_view_updates.load());
            r.put("tuner.view_seconds", g_view_seconds.load());
            r.put("tuner.player_camera", g_player_camera.load());
        }

        auto state(harness::Record& r) -> void override
        {
            r.put("debug.keep_follow", g_debug_keep_follow.load());
            r.put("debug.keep_turn", g_debug_keep_turn.load());
            r.put("debug.influence", g_debug_influence.load());
            r.put("debug.lag_h", g_debug_lag_h.load());
            r.put("debug.lag_v", g_debug_lag_v.load());
            r.put("debug.rate_h", g_debug_rate_h.load());
            r.put("debug.snap", g_debug_snap.load());
            r.put("debug.snap_qpc", g_debug_snap_qpc.load());
            r.put("debug.glide", g_debug_glide.load());
            r.put("hook.view_updates", g_view_updates.load());
            r.put("hook.view_seconds", g_view_seconds.load());
            r.put("hook.last_view_qpc", g_last_view_qpc.load());
            r.put("hook.reset", g_reset.load());
            r.put("hook.reset_reason", g_reset_reason.load());
            r.put("lua.enabled", dwapi::g_enabled && dwapi::g_enabled->load(std::memory_order_relaxed));
            ops::api_state(r);
        }

      private:
        uint64_t m_tuning_generation = 0; // the mod object's counter publish_locked bumped
    };
} // namespace

auto harness::make_base() -> harness::Variant*
{
    return new BaseDriver();
}
