// The split build: the core's hook side (camera/pipeline.hpp, with camera/authority.cpp linked in) and Smoothwalker's
// processor (smoothwalker/follow/processor.hpp), joined only through the core's interface (dw::camera::core_api(),
// what Core::api() returns), as the DLL runs them. The UE4SS-bound code that feeds them (core.cpp's constructor lines,
// smoothwalker.cpp's apply_position, update and api_status) is quoted below. Namespaces are renamed new_* on the
// command line so both builds link into one program.

#include "prelude.hpp" // first: see there

#include "camera/pipeline.hpp"
#include "smoothwalker/follow/processor.hpp"
#undef ifstream

#include "harness.hpp"

namespace
{
    using namespace dw::camera;

#include "new_api_ops.inc"

    class NewDriver final : public harness::Variant
    {
      public:
        auto init(const harness::HSettings& settings, bool enabled) -> void override
        {
            // Core::Core (core.cpp).
            g_authority.link(&processor_enabled, &g_reset, &g_reset_reason, &g_api_snapshot);
            QueryPerformanceFrequency(&g_qpc_frequency);
            g_original = &harness::original_view;
            ops::install(0);
            ops::install(1);
            // Smoothwalker::Impl's constructor (smoothwalker.cpp): register, then the startup publish and switch.
            m_core = &core_api();
            if (!m_core->register_processor(m_processor)) std::abort();
            publish(settings);
            m_processor.store_enabled(enabled);
        }

        auto publish(const harness::HSettings& h) -> void override
        {
            dw::smoothwalker::follow::publish_view(m_processor, *m_core, harness::to_settings<dw::smoothwalker::settings::Settings>(h));
        }

        auto set_enabled(bool on) -> void override { m_processor.set_enabled(on); }

        auto set_modes(bool aiming, bool combat, bool traversal, harness::Record& out) -> void override
        {
            // apply_position (smoothwalker.cpp).
            m_processor.set_aiming(aiming);
            out.put("modes.combat_was", m_processor.swap_combat(combat));
            out.put("modes.traversal_was", m_processor.swap_traversal(traversal));
        }

        auto mode_write() -> void override { m_processor.mode_written(); }

        auto request_cut(int reason) -> void override { dw::camera::request_cut(static_cast<dw::camera::Snap>(reason)); }

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
            // Smoothwalker::Impl::update's report (smoothwalker.cpp), over a fixed 5 s.
            r.put("stats.log_stats", m_processor.log_stats());
            auto stats = m_processor.take_stats();
            uint64_t frames = stats.frames;
            uint64_t clamped = stats.clamped;
            double lag = stats.lag_sum;
            uint64_t timed = 0;
            double micros = 0.0;
            m_core->take_hook_timing(timed, micros);
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
            // Smoothwalker::Impl::debug_panel and api_status (smoothwalker.cpp), then the keys' reads.
            r.put("panel.enabled", m_processor.enabled());
            const dw::smoothwalker::follow::FollowTuning follow = m_processor.tuning();
            r.put("panel.max_lag_h", follow.max_lag_h);
            r.put("panel.max_lag_v", follow.max_lag_v);
            r.put("panel.rotation_smoothing", follow.rotation_smoothing);
            const DebugFeed feed = m_core->read_debug();
            r.put("panel.keep_follow", feed.keep_follow);
            r.put("panel.keep_turn", feed.keep_turn);
            r.put("panel.influence", static_cast<int>(feed.influence));
            r.put("panel.lag_h", feed.lag_h);
            r.put("panel.lag_v", feed.lag_v);
            r.put("panel.rate_h", feed.rate_h);
            r.put("panel.snap", static_cast<int>(feed.snap));
            r.put("panel.snap_age", feed.snap_age);
            const Owner owner = m_core->camera_owner();
            const std::string& mod = owner.mod;
            const double lease = mod.empty() ? NAN : owner.lease;
            r.put("panel.owner", mod.data(), mod.size());
            r.put("panel.lease", lease);
            r.put("panel.glide", feed.glide);
            r.put("keys.camera_live", m_core->camera_live());
            r.put("tuner.view_updates", m_core->view_updates());
            r.put("tuner.view_seconds", m_core->view_seconds());
            r.put("tuner.player_camera", m_core->player_camera());
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
            r.put("lua.enabled", g_authority.enabled());
            ops::api_state(r);
        }

      private:
        CameraCore* m_core = nullptr;
        dw::smoothwalker::follow::FollowProcessor m_processor;
    };
} // namespace

auto harness::make_new() -> harness::Variant*
{
    return new NewDriver();
}
