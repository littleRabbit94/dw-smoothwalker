// The split build: the core's hook side (camera/hook.cpp's hook over camera/pipeline.cpp and camera/authority.cpp,
// linked in) and Smoothwalker's processor (smoothwalker/follow/processor.hpp), joined only through the core's
// interface (a CoreApi over the driver's Pipeline and Authority, what Core::api() returns), as the DLL runs them. The UE4SS-bound code that feeds them (core.cpp's constructor lines,
// smoothwalker.cpp's apply_position, update and api_status) is quoted below. Namespaces are renamed new_* on the
// command line so both builds link into one program.

#include "prelude.hpp" // first: see there

#include "camera/authority.hpp"
#include "camera/hook.hpp"
#include "camera/pipeline.hpp"
#include "smoothwalker/follow/processor.hpp"
#undef ifstream

#include "harness.hpp"

namespace
{
    using namespace dw::camera;

    // The harness's clock: one tick per now(), the same clock the baseline's QueryPerformanceCounter reads.
    constexpr Clock HARNESS_CLOCK{&harness::qpc_tick, &qpc_frequency};

    // The harness's guarded copy: fails the access the harness armed a fault for, as prelude.hpp's __try does for the
    // baseline.
    auto harness_copy(void* dst, const void* src, size_t n) -> bool
    {
        if (::harness::fault_now()) return false;
        std::memcpy(dst, src, n);
        return true;
    }

#include "new_api_ops.inc"

    class NewDriver final : public harness::Variant
    {
      public:
        // The hook's slot is image-level: nothing may reach this driver's Pipeline once it is gone.
        ~NewDriver() override
        {
            if (!unpublish_pipeline()) std::abort();
        }

        auto init(const harness::HSettings& settings, bool enabled) -> void override
        {
            // Core::Core (core.cpp): the members are constructed with the driver, the Pipeline goes to the hook.
            publish_pipeline(m_pipeline);
            // Core::Impl::install_hook (core.cpp), on a slot of the harness's that holds the game's GetCameraView.
            m_slot = reinterpret_cast<uintptr_t*>(&harness::original_view);
            if (!hook_slot(&m_slot)) std::abort();
            ops::install(m_authority, 0);
            ops::install(m_authority, 1);
            // Smoothwalker::Impl's constructor (smoothwalker.cpp): register, then the startup publish and switch.
            m_core = &m_api;
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

        auto request_cut(int reason) -> void override { m_pipeline.request_cut(static_cast<dw::camera::Snap>(reason)); }

        auto set_player(void* camera, void* root, int32_t translation_offset, int32_t half_height_offset) -> void override
        {
            m_pipeline.set_player_camera(camera);
            m_pipeline.set_player_root(root);
            m_pipeline.set_translation_offset(translation_offset);
            m_pipeline.set_half_height_offset(half_height_offset);
        }

        auto hook(void* self, float delta_time, void* desired_view) -> void override { get_camera_view_hook(self, delta_time, desired_view); }

        auto claim(int consumer, bool keep_layers, double ttl) -> int override { return ops::claim(m_authority, consumer, keep_layers, ttl); }
        auto release(int consumer, bool glide) -> int override { return ops::release(m_authority, consumer, glide); }
        auto uninstall(int consumer) -> void override { ops::uninstall(m_authority, consumer); }
        auto install(int consumer) -> void override { ops::install(m_authority, consumer); }
        auto layer_set(int consumer, const harness::LayerArgs& args) -> int override { return ops::layer_set(m_authority, consumer, args); }
        auto layer_clear(int consumer) -> int override { return ops::layer_clear(m_authority, consumer); }

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
            const Pipeline::Inspect p = m_pipeline.inspect();
            r.put("debug.keep_follow", p.keep_follow);
            r.put("debug.keep_turn", p.keep_turn);
            r.put("debug.influence", p.influence);
            r.put("debug.lag_h", p.lag_h);
            r.put("debug.lag_v", p.lag_v);
            r.put("debug.rate_h", p.rate_h);
            r.put("debug.snap", p.snap);
            r.put("debug.snap_qpc", p.snap_qpc);
            r.put("debug.glide", p.glide);
            r.put("hook.view_updates", p.view_updates);
            r.put("hook.view_seconds", p.view_seconds);
            r.put("hook.last_view_qpc", p.last_view_qpc);
            r.put("hook.reset", p.reset);
            r.put("hook.reset_reason", p.reset_reason);
            r.put("lua.enabled", m_authority.enabled());
            ops::api_state(r, m_pipeline, m_authority);
        }

      private:
        // Core::Impl's members (core.cpp), with the harness's clock and guarded copy.
        Authority m_authority{m_pipeline, HARNESS_CLOCK};
        Pipeline m_pipeline{m_authority, HARNESS_CLOCK, &harness_copy};
        CoreApi m_api{m_pipeline, m_authority};
        uintptr_t* m_slot = nullptr; // the harness's vtable slot 214
        CameraCore* m_core = nullptr;
        dw::smoothwalker::follow::FollowProcessor m_processor;
    };
} // namespace

auto harness::make_new() -> harness::Variant*
{
    return new NewDriver();
}
