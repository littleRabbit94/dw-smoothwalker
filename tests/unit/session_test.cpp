// session: seeded random sessions through the product as the DLL joins it, hashed against goldens. A Pipeline and an
// Authority with CoreApi over both, the FollowProcessor registered through that interface, the Pipeline published to
// camera/hook.cpp's slot and every camera update through get_camera_view_hook, on the test's clock (one tick per
// now()) and a guarded copy that fails the access a session event arms. The generator is the equivalence harness's
// (tests/equivalence, retired 2026-09-29, whose sessions matched commit f22aa8e byte for byte): walking, jumps,
// teleports, pauses, slow motion, crouches, bad half heights, NaN and infinite views, odd DeltaTime, other cameras,
// lost root / camera / offsets, faults on each guarded access, the toggle, publishes, mode writes, the aiming /
// combat / traversal flags, cuts for every reason, two API consumers (claims, leases, releases, uninstalls,
// re-keys, layers), log_stats takes and overlay reads.
//
// After every event its record (the view bytes the hook left, everything the hook publishes, the API's state and,
// when taken, the stats counters and the overlay's reads; every NaN of a double or float field as one value) and its
// count of clock reads go into one FNV-1a 64 hash per seed, which must equal tests/unit/session_golden.txt. Coverage
// summed over the seeds must reach every restart reason, crossfades, glides, faults, claims, releases, layer sets and
// re-keys.
//
// Re-recording: `dw_unit --record-session` rewrites session_golden.txt (the path is DW_REPO's), after checking
// coverage. The goldens are MSVC's (the standard library's distributions and the CRT's math belong to the compiler):
// record with this build only; Debug and Release give the same hashes. An intended change to the pipeline, the
// authority, the hook or the follow re-records in the commit that makes it, and that commit's message says so. A hash
// that changes without one is a regression.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "camera/authority.hpp"
#include "camera/hook.hpp"
#include "camera/pipeline.hpp"
#include "check.hpp"
#include "smoothwalker/follow/processor.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace dw::camera;

    constexpr uint64_t SEEDS = 8;       // seeds 1 to 8
    constexpr uint64_t FRAMES = 20'000; // per seed
    constexpr double FREQ = 1e7;        // clock ticks per second
    constexpr size_t VIEW_BUFFER = 96;  // the hook's view buffer: a write past FOV shows
    constexpr double INF = std::numeric_limits<double>::infinity();

    // ------------------------------------------------------------------------------------------ the session's world

    int64_t g_clock = 0;   // ticks; one per now()
    int g_fault_at = 0;    // the guarded access to fail in the current event, counted from 1; 0: none
    int g_fault_count = 0;
    alignas(16) uint8_t g_game_view[VIEW_BUFFER]{}; // what the game's GetCameraView writes this frame

    auto tick() -> int64_t
    {
        return g_clock++;
    }
    auto frequency() -> double
    {
        return FREQ;
    }
    constexpr Clock CLOCK{&tick, &frequency};

    auto guarded_copy(void* dst, const void* src, size_t n) -> bool
    {
        if (++g_fault_count == g_fault_at) return false;
        std::memcpy(dst, src, n);
        return true;
    }

    // The game's own GetCameraView, which the hook calls first: FMinimalViewInfo's head and more; the rest of the
    // buffer is the caller's.
    void __fastcall original_view(void*, float, void* view)
    {
        std::memcpy(view, g_game_view, 64);
    }

    // ------------------------------------------------------------------------------------------------ records

    // The settings the hook reads, by smoothwalker.ini key, with each session's starting values.
    struct HSettings
    {
        double follow_rate_h = 6.5, follow_rate_v = 10.0;
        int curve_h = 2, curve_v = 0;
        double catchup_distance = 150.0, min_rate_scale = 0.35, max_lag_h = 85.0, max_lag_v = 50.0;
        bool soft_leash = true;
        double aiming_follow = 30.0, combat_follow = 100.0, traversal_follow = 100.0;
        bool rotation_smoothing = false;
        double rotation_rate = 20.0, combat_rotation = 100.0, traversal_rotation = 100.0;
        bool wall_clamp = true;
        double reset_distance = 500.0, reset_gap = 0.25, position_transition = 0.5;
        bool log_stats = false, log_verbose = false;
    };

    auto to_settings(const HSettings& h) -> dw::smoothwalker::settings::Settings
    {
        dw::smoothwalker::settings::Settings s{};
        s.follow_rate_h = h.follow_rate_h;
        s.follow_rate_v = h.follow_rate_v;
        s.curve_h = h.curve_h;
        s.curve_v = h.curve_v;
        s.catchup_distance = h.catchup_distance;
        s.min_rate_scale = h.min_rate_scale;
        s.max_lag_h = h.max_lag_h;
        s.max_lag_v = h.max_lag_v;
        s.soft_leash = h.soft_leash;
        s.aiming_follow = h.aiming_follow;
        s.combat_follow = h.combat_follow;
        s.traversal_follow = h.traversal_follow;
        s.rotation_smoothing = h.rotation_smoothing;
        s.rotation_rate = h.rotation_rate;
        s.combat_rotation = h.combat_rotation;
        s.traversal_rotation = h.traversal_rotation;
        s.wall_clamp = h.wall_clamp;
        s.reset_distance = h.reset_distance;
        s.reset_gap = h.reset_gap;
        s.position_transition = h.position_transition;
        s.log_stats = h.log_stats;
        s.log_verbose = h.log_verbose;
        return s;
    }

    struct LayerArgs
    {
        double offset[3]{}, rotation[3]{};
        double fov = 0.0, fov_abs = NAN, weight = 1.0, blend = 0.0, ttl = 0.0;
    };

    // Bytes in the order they were put. The label names the field at the call site only: a hash has no per-field
    // report.
    struct Record
    {
        static constexpr size_t CAPACITY = 8192; // an event's record is about 2.4 KB
        uint8_t bytes[CAPACITY];
        size_t size = 0;

        auto put(const char*, const void* data, size_t n) -> void
        {
            if (n > CAPACITY - size) std::abort();
            std::memcpy(bytes + size, data, n);
            size += n;
        }
        template <typename T>
        auto put(const char* label, const T& value) -> void
        {
            put(label, &value, sizeof(T));
        }
        // Floating point with every NaN as one value: which NaN an operation on two NaNs returns (sign, payload)
        // follows the operand order, which the compiler may swap in a commutative operation. Whether a value is a
        // NaN, and every other bit, still counts.
        auto put(const char* label, double value) -> void
        {
            if (std::isnan(value)) value = std::numeric_limits<double>::quiet_NaN();
            put(label, &value, sizeof(value));
        }
        auto put(const char* label, float value) -> void
        {
            if (std::isnan(value)) value = std::numeric_limits<float>::quiet_NaN();
            put(label, &value, sizeof(value));
        }
        template <size_t N>
        auto put(const char* label, const double (&values)[N]) -> void
        {
            for (size_t i = 0; i < N; ++i) put(label, values[i]);
        }
        auto clear() -> void { size = 0; }
    };

    // FNV-1a, 64 bit.
    struct Fnv
    {
        uint64_t h = 14695981039346656037ull;
        auto add(const void* data, size_t n) -> void
        {
            auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
        }
    };

    // ----------------------------------------------------------------------------------- the API operations

    // The result codes the records carry, the reasons Lua would get.
    enum Code : int
    {
        Ok = 0,
        UnknownState,
        AlreadyYours,
        Taken,
        MustKeep,
        NotOwner,
        NotFinite,
        NoSlot,
    };

    // camera/lua_api.hpp's game-thread operations without Lua: its argument checks (a non-finite number is refused
    // before the Authority is called, as num_field makes l_claim and l_layer_set do), then the Authority's own method.
    // Consumers are fake keys; Lua is never entered.
    namespace ops
    {
        auto key_of(int consumer) -> const void*
        {
            return reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1000 + 0x100 * consumer));
        }

        const char* const CONSUMER_NAMES[] = {"ConsumerA", "ConsumerB"};

        auto code(ApiResult result) -> int
        {
            switch (result)
            {
            case ApiResult::Ok: return Ok;
            case ApiResult::UnknownState: return UnknownState;
            case ApiResult::AlreadyYours: return AlreadyYours;
            case ApiResult::Taken: return Taken;
            case ApiResult::MustKeep: return MustKeep;
            case ApiResult::NotOwner: return NotOwner;
            case ApiResult::NoSlot: return NoSlot;
            }
            std::abort();
        }

        auto install(Authority& api, int consumer) -> void { api.install(key_of(consumer), CONSUMER_NAMES[consumer], "0.10.1"); }

        auto uninstall(Authority& api, int consumer) -> void { api.uninstall(key_of(consumer)); }

        auto claim(Authority& api, int consumer, bool keep, double ttl) -> int
        {
            if (!std::isfinite(ttl)) return NotFinite;
            return code(api.claim(key_of(consumer), keep, ttl));
        }

        auto release(Authority& api, int consumer, bool glide) -> int { return code(api.release(key_of(consumer), glide)); }

        auto layer_set(Authority& api, int consumer, const LayerArgs& a) -> int
        {
            bool bad = false;
            auto number = [&](double v) {
                if (!std::isfinite(v)) bad = true;
                return v;
            };
            Layer l;
            for (int k = 0; k < 3; ++k) l.offset[k] = number(a.offset[k]);
            for (int k = 0; k < 3; ++k) l.rotation[k] = number(a.rotation[k]);
            l.fov_delta = number(a.fov);
            l.fov_abs = std::isnan(a.fov_abs) ? NAN : number(a.fov_abs); // absent is NAN; present must be finite
            l.weight = number(a.weight);
            l.blend = number(a.blend);
            double ttl = number(a.ttl);
            if (bad) return NotFinite;
            return code(api.layer_set(key_of(consumer), l, ttl));
        }

        auto layer_clear(Authority& api, int consumer) -> int { return code(api.layer_clear(key_of(consumer))); }

        // Everything the API side holds that the hook reads or writes, field by field (both structs have padding).
        // Answers the blending flag, for the coverage counters.
        auto api_state(Record& r, const Pipeline& pipeline, const Authority& api) -> bool
        {
            r.put("api.seq", pipeline.snapshot().seq());
            const Snapshot& snapshot = pipeline.snapshot().data();
            auto view = [&](const char* label, const View& v) {
                r.put(label, v.location);
                r.put(label, v.rotation);
                r.put(label, v.fov);
            };
            view("api.snapshot.game", snapshot.game);
            view("api.snapshot.shown", snapshot.shown);
            r.put("api.snapshot.pivot", snapshot.pivot);
            r.put("api.snapshot.half_height", snapshot.half_height);
            r.put("api.snapshot.follow_offset", snapshot.follow_offset);
            r.put("api.snapshot.follow_yaw", snapshot.follow_yaw);
            r.put("api.snapshot.qpc", snapshot.qpc);
            const Authority::Inspect a = api.inspect();
            r.put("api.blending", a.blending);
            r.put("api.layers_any", a.layers_any);
            r.put("api.owner_slot", a.owner_slot);
            r.put("api.owner_expires", a.owner_expires);
            r.put("api.owner_keep_layers", a.owner_keep_layers);
            r.put("api.release_generation", a.release_generation);
            r.put("api.layer_generation", a.layer_generation);
            for (const auto& l : a.layers)
            {
                r.put("layer.active", l.active);
                r.put("layer.offset", l.offset);
                r.put("layer.rotation", l.rotation);
                r.put("layer.fov_delta", l.fov_delta);
                r.put("layer.fov_abs", l.fov_abs);
                r.put("layer.weight", l.weight);
                r.put("layer.blend", l.blend);
                r.put("layer.expires", l.expires);
                r.put("layer.generation", l.generation);
            }
            for (const auto& st : a.layer_state)
            {
                r.put("layer_state.cur", st.cur);
                r.put("layer_state.from", st.from);
                r.put("layer_state.elapsed", st.elapsed);
                r.put("layer_state.duration", st.duration);
                r.put("layer_state.seen", st.seen);
                r.put("layer_state.was_active", st.was_active);
                r.put("layer_state.last_blend", st.last_blend);
                r.put("layer_state.idle", st.idle);
            }
            r.put("api.owner_state", reinterpret_cast<uintptr_t>(a.owner_state)); // a fake key: the same every run
            r.put("api.owner_mod", a.owner_mod.data(), a.owner_mod.size());
            r.put("api.consumers", a.consumers);
            return a.blending;
        }
    } // namespace ops

    // ------------------------------------------------------------------------------------------------ the product

    // Core::Impl's members (core.cpp) with the test's clock and guarded copy, the hook on a slot of the test's, and
    // the FollowProcessor registered through CoreApi as Smoothwalker registers it. The UE4SS-bound feeding code
    // (smoothwalker.cpp's apply_position, update's report, debug_panel and api_status) is quoted.
    class Driver
    {
      public:
        Driver() = default;
        Driver(const Driver&) = delete;
        auto operator=(const Driver&) -> Driver& = delete;

        // The hook's slot is image-level: nothing may reach this Pipeline once it is gone.
        ~Driver()
        {
            if (!unpublish_pipeline()) std::abort();
        }

        auto init(const HSettings& settings, bool enabled) -> void
        {
            // Core::Core (core.cpp): the Pipeline goes to the hook; Core::Impl::install_hook, on the test's slot that
            // holds the game's GetCameraView.
            publish_pipeline(m_pipeline);
            m_slot = reinterpret_cast<uintptr_t*>(&original_view);
            if (!hook_slot(&m_slot)) std::abort();
            ops::install(m_authority, 0);
            ops::install(m_authority, 1);
            // Smoothwalker::Impl's constructor (smoothwalker.cpp): register, then the startup publish and switch.
            m_core = &m_api;
            if (!m_core->register_processor(m_processor)) std::abort();
            publish(settings);
            m_processor.store_enabled(enabled);
        }

        auto publish(const HSettings& h) -> void { dw::smoothwalker::follow::publish_view(m_processor, *m_core, to_settings(h)); }

        auto set_enabled(bool on) -> void { m_processor.set_enabled(on); }

        auto set_modes(bool aiming, bool combat, bool traversal, Record& out) -> void
        {
            // apply_position (smoothwalker.cpp).
            m_processor.set_aiming(aiming);
            out.put("modes.combat_was", m_processor.swap_combat(combat));
            out.put("modes.traversal_was", m_processor.swap_traversal(traversal));
        }

        auto mode_write() -> void { m_processor.mode_written(); }

        auto request_cut(int reason) -> void { m_pipeline.request_cut(static_cast<Snap>(reason)); }

        auto set_player(void* camera, void* root, int32_t translation_offset, int32_t half_height_offset) -> void
        {
            m_pipeline.set_player_camera(camera);
            m_pipeline.set_player_root(root);
            m_pipeline.set_translation_offset(translation_offset);
            m_pipeline.set_half_height_offset(half_height_offset);
        }

        auto hook(void* self, float delta_time, void* desired_view) -> void
        {
            // Through the test's slot, as the game's vtable call lands.
            reinterpret_cast<GetCameraViewFn>(m_slot)(self, delta_time, desired_view);
        }

        auto claim(int consumer, bool keep_layers, double ttl) -> int { return ops::claim(m_authority, consumer, keep_layers, ttl); }
        auto release(int consumer, bool glide) -> int { return ops::release(m_authority, consumer, glide); }
        auto uninstall(int consumer) -> void { ops::uninstall(m_authority, consumer); }
        auto install(int consumer) -> void { ops::install(m_authority, consumer); }
        auto layer_set(int consumer, const LayerArgs& args) -> int { return ops::layer_set(m_authority, consumer, args); }
        auto layer_clear(int consumer) -> int { return ops::layer_clear(m_authority, consumer); }

        auto take_stats(Record& r) -> void
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
            r.put("stats.rate", static_cast<double>(frames) / elapsed);
            r.put("stats.mean_lag", frames ? lag / static_cast<double>(frames) : 0.0);
            r.put("stats.clamp_share", frames ? 100.0 * static_cast<double>(clamped) / static_cast<double>(frames) : 0.0);
        }

        // `camera`: the session's player camera, so the record holds which object the core names, not its address.
        auto panel(Record& r, const void* camera) -> void
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
            r.put("panel.snap_time", feed.snap_time);
            const Owner owner = m_core->camera_owner();
            const std::string& mod = owner.mod;
            const double lease = mod.empty() ? NAN : owner.lease;
            r.put("panel.owner", mod.data(), mod.size());
            r.put("panel.lease", lease);
            r.put("panel.glide", feed.glide);
            r.put("keys.camera_live", m_core->camera_live());
            r.put("tuner.view_updates", m_core->view_updates());
            r.put("tuner.view_seconds", m_core->view_seconds());
            const void* player = m_core->player_camera();
            r.put("tuner.player_camera", player == nullptr ? 0 : player == camera ? 1 : 2);
        }

        auto state(Record& r) -> void
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
            const bool blending = ops::api_state(r, m_pipeline, m_authority);
            m_seen = {p.snap, p.snap_qpc, p.glide, blending};
        }

        // What the last state() recorded of the last restart and the crossfade, for the coverage counters.
        struct Seen
        {
            int snap = 0;
            int64_t snap_qpc = 0;
            bool glide = false, blending = false;
        };
        auto seen() const -> const Seen& { return m_seen; }

      private:
        Authority m_authority{m_pipeline, CLOCK};
        Pipeline m_pipeline{m_authority, CLOCK, &guarded_copy};
        CoreApi m_api{m_pipeline, m_authority};
        uintptr_t* m_slot = nullptr; // the test's vtable slot 214
        CameraCore* m_core = nullptr;
        dw::smoothwalker::follow::FollowProcessor m_processor;
        Seen m_seen;
    };

    // --------------------------------------------------------------------------------------------- the session

    struct Counters
    {
        uint64_t events = 0, hook_calls = 0, player_updates = 0, other_calls = 0, publishes = 0, toggles = 0, mode_writes = 0,
                 cuts_requested = 0, claims_ok = 0, claims_refused = 0, renewals = 0, releases_glide = 0, releases_cut = 0, uninstalls = 0,
                 rekeys = 0, layer_sets = 0, layer_refused = 0, layer_no_slot = 0, layer_clears = 0, stats_takes = 0, panels = 0, faults = 0,
                 nan_views = 0, pauses = 0, teleports = 0, crouches = 0, bad_half_heights = 0, crossfades = 0, glides = 0,
                 transition_zero_publishes = 0;
        std::map<int, uint64_t> snaps; // Snap -> restarts seen

        auto add(const Counters& o) -> void
        {
            uint64_t* const mine[] = {&events, &hook_calls, &player_updates, &other_calls, &publishes, &toggles, &mode_writes, &cuts_requested,
                                      &claims_ok, &claims_refused, &renewals, &releases_glide, &releases_cut, &uninstalls, &rekeys, &layer_sets,
                                      &layer_refused, &layer_no_slot, &layer_clears, &stats_takes, &panels, &faults, &nan_views, &pauses,
                                      &teleports, &crouches, &bad_half_heights, &crossfades, &glides, &transition_zero_publishes};
            const uint64_t* const theirs[] = {&o.events, &o.hook_calls, &o.player_updates, &o.other_calls, &o.publishes, &o.toggles, &o.mode_writes,
                                              &o.cuts_requested, &o.claims_ok, &o.claims_refused, &o.renewals, &o.releases_glide, &o.releases_cut,
                                              &o.uninstalls, &o.rekeys, &o.layer_sets, &o.layer_refused, &o.layer_no_slot, &o.layer_clears,
                                              &o.stats_takes, &o.panels, &o.faults, &o.nan_views, &o.pauses, &o.teleports, &o.crouches,
                                              &o.bad_half_heights, &o.crossfades, &o.glides, &o.transition_zero_publishes};
            static_assert(std::size(mine) == std::size(theirs));
            for (size_t i = 0; i < std::size(mine); ++i) *mine[i] += *theirs[i];
            for (const auto& [snap, n] : o.snaps) snaps[snap] += n;
        }
    };

    const char* const SNAP_NAMES[] = {"None", "Startup", "Teleport", "Gap", "World", "Player", "Pawn", "Toggle", "ApiCut", "ClaimEnded", "ViewLost"};

    class Session
    {
      public:
        explicit Session(uint64_t seed) : m_rng(seed)
        {
            std::memset(m_root, 0, sizeof(m_root));
            m_settings = random_settings(HSettings{});
            event("init", [&](Record&) { m_driver.init(m_settings, m_enabled); });
            place_player();
        }

        auto run(uint64_t frames) -> void
        {
            for (m_frame = 0; m_frame < frames; ++m_frame) frame();
        }

        auto hash() const -> uint64_t { return m_hash.h; }
        auto counters() const -> const Counters& { return m_counters; }

      private:
        std::mt19937_64 m_rng;
        Driver m_driver;
        Record m_r;
        Fnv m_hash;
        Counters m_counters;
        uint64_t m_frame = 0;
        int64_t m_last_snap_qpc = 0;
        bool m_last_blending = false;

        // The world.
        HSettings m_settings;
        bool m_enabled = true;
        bool m_aiming = false, m_combat = false, m_traversal = false;
        double m_pos[3]{1000.0, -2000.0, 96.0}; // capsule centre
        double m_vel[3]{};
        double m_ground = 0.0;
        double m_half = 96.0;
        int m_bad_half_frames = 0;
        double m_yaw = 0.0, m_pitch = -10.0, m_roll = 0.0, m_yaw_rate = 0.0, m_pitch_rate = 0.0;
        int m_spin_frames = 0, m_roll_frames = 0;
        double m_arm = 250.0, m_arm_target = 250.0;
        int m_wall_frames = 0;
        double m_fov = 90.0;
        double m_slowmo = 1.0;
        int m_slowmo_frames = 0;
        int m_camera_gone = 0, m_root_gone = 0, m_offset_gone = 0;
        bool m_half_offset_missing = false;
        bool m_installed[2]{true, true};
        double m_rekey_chance = 0.00001; // per consumer and frame
        uint8_t m_root[0x800]; // the player root; read through the guarded copy, so any alignment
        uint8_t m_camera_object = 0, m_other_object = 0;
        static constexpr int32_t TRANSLATION = 0x200, HALF = 0x1F0;

        auto uniform(double a, double b) -> double { return std::uniform_real_distribution<double>(a, b)(m_rng); }
        auto chance(double p) -> bool { return uniform(0.0, 1.0) < p; }
        auto pick(int n) -> int { return std::uniform_int_distribution<int>(0, n - 1)(m_rng); }

        // Applies one event, then hashes its name, its clock reads and the record of what the product holds after it.
        template <typename Apply>
        auto event(const char* what, Apply&& apply, int fault_at = 0) -> void
        {
            const int64_t t0 = g_clock;
            m_r.clear();
            g_fault_at = fault_at;
            g_fault_count = 0;
            apply(m_r);
            m_driver.state(m_r);
            g_fault_at = 0;
            ++m_counters.events;
            const int64_t reads = g_clock - t0;
            m_hash.add(what, std::strlen(what) + 1);
            m_hash.add(&reads, sizeof(reads));
            m_hash.add(m_r.bytes, m_r.size);
        }

        auto random_settings(HSettings s) -> HSettings
        {
            // Values inside the Mod Menu's ranges, the way sanitize() leaves them.
            auto maybe = [&](double p) { return chance(p); };
            if (maybe(0.5)) s.follow_rate_h = uniform(0.5, 30.0);
            if (maybe(0.5)) s.follow_rate_v = uniform(0.5, 30.0);
            if (maybe(0.3)) s.curve_h = pick(4);
            if (maybe(0.3)) s.curve_v = pick(4);
            if (maybe(0.3)) s.catchup_distance = uniform(10.0, 500.0);
            if (maybe(0.3)) s.min_rate_scale = uniform(0.05, 1.0);
            if (maybe(0.4)) s.max_lag_h = chance(0.1) ? 0.0 : uniform(0.0, 300.0);
            if (maybe(0.4)) s.max_lag_v = chance(0.1) ? 0.0 : uniform(0.0, 200.0);
            if (maybe(0.3)) s.soft_leash = chance(0.7);
            if (maybe(0.3)) s.aiming_follow = uniform(0.0, 100.0);
            if (maybe(0.3)) s.combat_follow = uniform(0.0, 100.0);
            if (maybe(0.3)) s.traversal_follow = uniform(0.0, 100.0);
            if (maybe(0.3)) s.rotation_smoothing = chance(0.5);
            if (maybe(0.3)) s.rotation_rate = uniform(1.0, 60.0);
            if (maybe(0.3)) s.combat_rotation = uniform(0.0, 100.0);
            if (maybe(0.3)) s.traversal_rotation = uniform(0.0, 100.0);
            if (maybe(0.2)) s.wall_clamp = chance(0.8);
            if (maybe(0.2)) s.reset_distance = uniform(100.0, 3000.0);
            if (maybe(0.2)) s.reset_gap = uniform(0.05, 2.0);
            if (maybe(0.3)) s.position_transition = chance(0.2) ? 0.0 : uniform(0.0, 2.0);
            if (maybe(0.2)) s.log_stats = !s.log_stats;
            if (maybe(0.1)) s.log_verbose = !s.log_verbose;
            return s;
        }

        auto place_player() -> void
        {
            void* camera = m_camera_gone == 0 ? &m_camera_object : nullptr;
            void* root = camera && m_root_gone == 0 ? m_root : nullptr;
            int32_t translation = m_offset_gone > 0 ? -1 : TRANSLATION;
            int32_t half = m_half_offset_missing ? -1 : HALF;
            event("set_player", [&](Record&) { m_driver.set_player(camera, root, translation, half); });
        }

        // The game's view this frame: location, rotation, FOV, then bytes the hook must leave alone.
        auto build_view(bool nan_view) -> void
        {
            const double d2r = 3.14159265358979323846 / 180.0;
            double cy = std::cos(m_yaw * d2r), sy = std::sin(m_yaw * d2r), cp = std::cos(m_pitch * d2r), sp = std::sin(m_pitch * d2r);
            double forward[3]{cp * cy, cp * sy, sp};
            double right[3]{-sy, cy, 0.0};
            double pivot[3]{m_pos[0], m_pos[1], m_pos[2] + 75.0};
            double view[7];
            for (int k = 0; k < 3; ++k) view[k] = pivot[k] - forward[k] * m_arm + right[k] * 40.0;
            view[3] = m_pitch;
            view[4] = m_yaw;
            view[5] = m_roll;
            float fov = static_cast<float>(m_fov);
            if (nan_view)
            {
                int which = pick(4);
                if (which == 0) view[pick(3)] = NAN;
                else if (which == 1) view[3 + pick(3)] = NAN;
                else if (which == 2) view[pick(3)] = INF;
                else fov = NAN;
            }
            for (size_t i = 0; i < sizeof(g_game_view); ++i) g_game_view[i] = static_cast<uint8_t>(0xA5 ^ i);
            std::memcpy(g_game_view, view, 48);
            std::memcpy(g_game_view + 48, &fov, 4);
        }

        auto write_root() -> void
        {
            double half = m_half;
            if (m_bad_half_frames > 0)
            {
                static const double bad[] = {NAN, -5.0, 5000.0, INF, 1000.5};
                half = bad[m_frame % 5];
            }
            std::memcpy(m_root + TRANSLATION, m_pos, sizeof(m_pos));
            float h = static_cast<float>(half);
            std::memcpy(m_root + HALF, &h, sizeof(h));
        }

        auto hook_call(void* self, float delta_time, int fault_at) -> void
        {
            ++m_counters.hook_calls;
            if (self == &m_camera_object) ++m_counters.player_updates;
            else ++m_counters.other_calls;
            event(
                    self == &m_camera_object ? "hook(player)" : "hook(other)",
                    [&](Record& r) {
                        alignas(16) uint8_t view[VIEW_BUFFER];
                        for (size_t i = 0; i < VIEW_BUFFER; ++i) view[i] = static_cast<uint8_t>(0x3C + i);
                        m_driver.hook(self, delta_time, view);
                        r.put("view", view, VIEW_BUFFER);
                    },
                    fault_at);
            // Coverage: a restart, a crossfade starting.
            const Driver::Seen& seen = m_driver.seen();
            const int snap = seen.snap;
            const int64_t snap_qpc = seen.snap_qpc;
            const bool blending = seen.blending;
            const bool glide = seen.glide;
            if (snap_qpc != m_last_snap_qpc) ++m_counters.snaps[snap];
            m_last_snap_qpc = snap_qpc;
            if (blending && !m_last_blending) ++m_counters.crossfades, m_counters.glides += glide ? 1 : 0;
            m_last_blending = blending;
        }

        auto game_thread_events() -> void
        {
            // Settings.
            if (chance(0.003))
            {
                ++m_counters.publishes;
                if (!chance(0.25)) m_settings = random_settings(m_settings); // else the same values again: no fade
                if (m_settings.position_transition == 0.0) ++m_counters.transition_zero_publishes;
                event("publish", [&](Record&) { m_driver.publish(m_settings); });
            }
            if (chance(0.002))
            {
                ++m_counters.toggles;
                m_enabled = !m_enabled;
                event("toggle", [&](Record&) { m_driver.set_enabled(m_enabled); });
            }
            if (chance(0.0005)) event("toggle(same)", [&](Record&) { m_driver.set_enabled(m_enabled); });
            // Camera modes.
            if (chance(0.01)) m_aiming = !m_aiming;
            if (chance(0.006)) m_combat = !m_combat;
            if (chance(0.006)) m_traversal = !m_traversal;
            event("modes", [&](Record& r) { m_driver.set_modes(m_aiming, m_combat, m_traversal, r); });
            if (chance(0.003))
            {
                ++m_counters.mode_writes;
                event("mode_write", [&](Record&) { m_driver.mode_write(); });
            }
            // The core's own cuts: a level change, a controller or a pawn.
            if (chance(0.0015))
            {
                ++m_counters.cuts_requested;
                static const int reasons[] = {4, 5, 6}; // World, Player, Pawn
                int reason = reasons[pick(3)];
                event("request_cut", [&](Record&) { m_driver.request_cut(reason); });
                if (reason != 6 && chance(0.5))
                {
                    // The player forgotten for a while: no updates reach the pipeline.
                    m_camera_gone = 1 + pick(200);
                    place_player();
                }
            }
            // The camera API: two consumers.
            for (int c = 0; c < 2; ++c)
            {
                if (!m_installed[c])
                {
                    if (chance(0.01))
                    {
                        m_installed[c] = true;
                        event("install", [&](Record&) { m_driver.install(c); });
                    }
                    continue;
                }
                if (chance(0.003))
                {
                    bool keep = chance(0.5);
                    double ttl = chance(0.5) ? 0.0 : chance(0.02) ? NAN : uniform(0.05, 3.0);
                    int code = 0;
                    event("claim", [&](Record& r) {
                        code = m_driver.claim(c, keep, ttl);
                        r.put("claim", code);
                    });
                    if (code == Ok) ++m_counters.claims_ok;
                    else if (code == AlreadyYours) ++m_counters.renewals;
                    else ++m_counters.claims_refused;
                }
                if (chance(0.0035))
                {
                    bool glide = chance(0.5);
                    int code = 0;
                    event("release", [&](Record& r) {
                        code = m_driver.release(c, glide);
                        r.put("release", code);
                    });
                    if (code == Ok) ++(glide ? m_counters.releases_glide : m_counters.releases_cut);
                }
                if (chance(0.0003))
                {
                    ++m_counters.uninstalls;
                    m_installed[c] = false;
                    event("uninstall", [&](Record&) { m_driver.uninstall(c); });
                    continue;
                }
                // Rare: a re-key without on_lua_stop. It frees the layer slot the consumer held (Authority::install).
                if (chance(m_rekey_chance))
                {
                    ++m_counters.rekeys;
                    event("install(re-key)", [&](Record&) { m_driver.install(c); });
                }
                if (chance(0.004))
                {
                    LayerArgs a;
                    for (int k = 0; k < 3; ++k) a.offset[k] = chance(0.5) ? 0.0 : uniform(-100.0, 100.0);
                    for (int k = 0; k < 3; ++k) a.rotation[k] = chance(0.5) ? 0.0 : uniform(-20.0, 20.0);
                    a.fov = chance(0.5) ? 0.0 : uniform(-20.0, 20.0);
                    a.fov_abs = chance(0.7) ? NAN : uniform(0.0, 200.0);
                    a.weight = uniform(-0.2, 1.2);
                    a.blend = chance(0.3) ? 0.0 : uniform(-0.5, 2.0);
                    a.ttl = chance(0.5) ? 0.0 : uniform(0.05, 3.0);
                    if (chance(0.03)) a.offset[pick(3)] = INF;
                    int code = 0;
                    event("layer_set", [&](Record& r) {
                        code = m_driver.layer_set(c, a);
                        r.put("layer_set", code);
                    });
                    if (code == Ok) ++m_counters.layer_sets;
                    else if (code == NoSlot) ++m_counters.layer_no_slot;
                    else ++m_counters.layer_refused;
                }
                if (chance(0.002))
                {
                    ++m_counters.layer_clears;
                    event("layer_clear", [&](Record& r) { r.put("layer_clear", m_driver.layer_clear(c)); });
                }
            }
        }

        auto move_world(double dt) -> void
        {
            // Walking: a random walk on the velocity, speeds up to about 9 m/s; jumps and gravity.
            for (int k = 0; k < 2; ++k)
            {
                m_vel[k] += uniform(-900.0, 900.0) * dt;
                m_vel[k] = std::clamp(m_vel[k], -900.0, 900.0);
                if (chance(0.002)) m_vel[k] = 0.0;
                m_pos[k] += m_vel[k] * dt;
            }
            double feet = m_pos[2] - m_half;
            if (feet <= m_ground + 1e-9 && chance(0.004)) m_vel[2] = 450.0;
            m_vel[2] -= 980.0 * dt;
            feet += m_vel[2] * dt;
            if (feet < m_ground)
            {
                feet = m_ground;
                m_vel[2] = 0.0;
            }
            if (chance(0.001)) m_ground += uniform(-30.0, 30.0); // steps and slopes
            if (chance(0.004))
            {
                ++m_counters.crouches;
                m_half = m_half > 60.0 ? 42.0 : 96.0; // the centre moves, the feet stay
            }
            m_pos[2] = feet + m_half;
            if (chance(0.001))
            {
                ++m_counters.teleports;
                static const double sizes[] = {150.0, 450.0, 520.0, 900.0, 3500.0};
                double size = sizes[pick(5)];
                double a = uniform(0.0, 6.283185307179586);
                m_pos[0] += size * std::cos(a);
                m_pos[1] += size * std::sin(a);
            }
            if (m_bad_half_frames > 0) --m_bad_half_frames;
            else if (chance(0.002))
            {
                ++m_counters.bad_half_heights;
                m_bad_half_frames = 1 + pick(5);
            }

            // Looking: turn rates on a random walk, fast spins, one-frame flips, some roll.
            m_yaw_rate += uniform(-600.0, 600.0) * dt;
            m_yaw_rate = std::clamp(m_yaw_rate, -250.0, 250.0);
            m_pitch_rate += uniform(-300.0, 300.0) * dt;
            m_pitch_rate = std::clamp(m_pitch_rate, -120.0, 120.0);
            if (m_spin_frames > 0) --m_spin_frames;
            else if (chance(0.002)) m_spin_frames = 20 + pick(60);
            m_yaw += (m_spin_frames > 0 ? 720.0 : m_yaw_rate) * dt;
            if (chance(0.001)) m_yaw += uniform(150.0, 210.0);
            m_yaw = std::remainder(m_yaw, 360.0);
            m_pitch = std::clamp(m_pitch + m_pitch_rate * dt, -89.0, 89.0);
            if (m_roll_frames > 0 && --m_roll_frames == 0) m_roll = 0.0;
            else if (chance(0.001))
            {
                m_roll = uniform(-30.0, 30.0);
                m_roll_frames = 1 + pick(100);
            }

            // The arm: walls pull it in, aiming and close combat shorten it, sometimes in one frame.
            if (m_wall_frames > 0 && --m_wall_frames == 0) m_arm_target = 250.0;
            else if (chance(0.003))
            {
                m_arm_target = 250.0 * uniform(0.25, 0.95);
                m_wall_frames = 10 + pick(120);
            }
            m_arm = chance(0.3) ? m_arm_target : m_arm + (m_arm_target - m_arm) * std::min(1.0, 8.0 * dt);
            if (chance(0.002)) m_fov = uniform(60.0, 120.0);
            else if (chance(0.0005)) m_fov = chance(0.5) ? 0.5 : 200.0;
            else m_fov += uniform(-0.2, 0.2);
        }

        auto frame() -> void
        {
            // A pause (the camera stops updating), then this frame's time.
            if (chance(0.0015))
            {
                ++m_counters.pauses;
                g_clock += static_cast<int64_t>(uniform(0.1, 5.0) * FREQ);
            }
            double dt = chance(0.01) ? uniform(0.1, 0.4) : uniform(1.0 / 144.0, 1.0 / 30.0);
            g_clock += static_cast<int64_t>(dt * FREQ);
            if (m_slowmo_frames > 0 && --m_slowmo_frames == 0) m_slowmo = 1.0;
            else if (chance(0.001))
            {
                m_slowmo = uniform(0.05, 0.8);
                m_slowmo_frames = 10 + pick(300);
            }
            move_world(dt * m_slowmo);

            // The player coming and going: the pawn, the root, the translation offset, the half-height offset.
            bool replace = false;
            if (m_camera_gone > 0 && --m_camera_gone == 0) replace = true;
            if (m_root_gone > 0 && --m_root_gone == 0) replace = true;
            if (m_offset_gone > 0 && --m_offset_gone == 0) replace = true;
            if (m_camera_gone == 0 && chance(0.0008))
            {
                m_camera_gone = 1 + pick(300);
                event("request_cut", [&](Record&) { m_driver.request_cut(6); }); // forget_pawn
                replace = true;
            }
            if (m_root_gone == 0 && chance(0.0005)) m_root_gone = 1 + pick(60), replace = true;
            if (m_offset_gone == 0 && chance(0.0003)) m_offset_gone = 1 + pick(60), replace = true;
            if (chance(0.0005)) m_half_offset_missing = !m_half_offset_missing, replace = true;
            if (replace) place_player();

            game_thread_events();

            // The camera updates: other cameras around the player's.
            write_root();
            bool nan_view = chance(0.002);
            if (nan_view) ++m_counters.nan_views;
            build_view(nan_view);
            float delta_time = static_cast<float>(dt * m_slowmo);
            if (chance(0.003))
            {
                static const float odd[] = {0.0f, -0.01f, NAN, std::numeric_limits<float>::infinity(), 1.0f};
                delta_time = odd[pick(5)];
            }
            if (chance(0.3)) hook_call(&m_other_object, delta_time, 0);
            int fault = 0;
            if (chance(0.003))
            {
                ++m_counters.faults;
                fault = 1 + pick(4);
            }
            hook_call(&m_camera_object, delta_time, fault);
            if (chance(0.2)) hook_call(&m_other_object, delta_time, 0);

            // The game thread reading back: the log_stats report, the overlay and the keys.
            if (chance(0.01))
            {
                ++m_counters.stats_takes;
                event("take_stats", [&](Record& r) { m_driver.take_stats(r); });
            }
            if (chance(0.01))
            {
                ++m_counters.panels;
                event("panel", [&](Record& r) { m_driver.panel(r, &m_camera_object); });
            }
        }
    };

    // ------------------------------------------------------------------------------------ runs and goldens

    struct Golden
    {
        uint64_t seed, frames, hash;
    };

    struct Results
    {
        std::vector<Golden> hashes; // seeds 1 to SEEDS
        Counters total;
    };

    auto run_all() -> Results
    {
        Results out;
        for (uint64_t seed = 1; seed <= SEEDS; ++seed)
        {
            g_clock = 1'000'000'000; // 100 s of QPC ticks, a session's start
            g_fault_at = 0;
            g_fault_count = 0;
            uint64_t hash = 0;
            {
                Session session(seed);
                session.run(FRAMES);
                hash = session.hash();
                out.total.add(session.counters());
                std::printf("  seed %" PRIu64 ": %" PRIu64 " frames, %" PRIu64 " events, hash %016" PRIx64 "\n", seed, FRAMES,
                            session.counters().events, hash);
            }
            out.hashes.push_back({seed, FRAMES, hash});
        }
        const Counters& c = out.total;
        std::printf("  hook calls %" PRIu64 " (player %" PRIu64 ", other %" PRIu64 "), NaN views %" PRIu64 ", faults %" PRIu64 ", pauses %" PRIu64
                    ", teleports %" PRIu64 ", crouches %" PRIu64 ", bad half heights %" PRIu64 "\n",
                    c.hook_calls, c.player_updates, c.other_calls, c.nan_views, c.faults, c.pauses, c.teleports, c.crouches, c.bad_half_heights);
        std::printf("  publishes %" PRIu64 " (transition 0: %" PRIu64 "), toggles %" PRIu64 ", mode writes %" PRIu64 ", cuts requested %" PRIu64
                    ", crossfades %" PRIu64 " (glides %" PRIu64 ")\n",
                    c.publishes, c.transition_zero_publishes, c.toggles, c.mode_writes, c.cuts_requested, c.crossfades, c.glides);
        std::printf("  claims %" PRIu64 " (refused %" PRIu64 ", renewals %" PRIu64 "), releases glide %" PRIu64 " / cut %" PRIu64 ", uninstalls %" PRIu64
                    ", re-keys %" PRIu64 ", layer sets %" PRIu64 " (not finite %" PRIu64 ", no slot %" PRIu64 "), clears %" PRIu64 ", stats takes %" PRIu64
                    ", panel reads %" PRIu64 "\n",
                    c.claims_ok, c.claims_refused, c.renewals, c.releases_glide, c.releases_cut, c.uninstalls, c.rekeys, c.layer_sets, c.layer_refused,
                    c.layer_no_slot, c.layer_clears, c.stats_takes, c.panels);
        std::printf("  restarts by reason:");
        for (const auto& [snap, n] : c.snaps) std::printf(" %s %" PRIu64, snap >= 0 && snap <= 10 ? SNAP_NAMES[snap] : "?", n);
        std::printf("\n");
        return out;
    }

    // Every restart reason, crossfades and glides, faults, claims, releases, layer sets and re-keys. Counts failures.
    auto check_coverage(const Counters& c) -> int
    {
        int missing = 0;
        auto need = [&](bool ok, const char* what) {
            if (ok) return;
            ++missing;
            std::printf("  coverage: no %s\n", what);
        };
        for (int snap = static_cast<int>(Snap::Startup); snap <= static_cast<int>(Snap::ViewLost); ++snap)
        {
            auto it = c.snaps.find(snap);
            need(it != c.snaps.end() && it->second > 0, SNAP_NAMES[snap]);
        }
        need(c.crossfades > 0, "crossfade");
        need(c.glides > 0, "glide");
        need(c.faults > 0, "injected fault");
        need(c.claims_ok > 0, "claim");
        need(c.releases_glide + c.releases_cut > 0, "release");
        need(c.layer_sets > 0, "layer set");
        need(c.rekeys > 0, "re-key");
        return missing;
    }

    auto golden_path() -> std::string
    {
        return std::string(DW_REPO) + "/tests/unit/session_golden.txt";
    }

    // Lines of `seed frames hash` (hash in hex); `#` starts a comment line.
    auto read_goldens(std::vector<Golden>& out) -> bool
    {
        std::ifstream file(golden_path());
        if (!file) return false;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#' || line[0] == '\r') continue;
            std::istringstream in(line);
            Golden g{};
            if (!(in >> g.seed >> g.frames >> std::hex >> g.hash)) return false;
            out.push_back(g);
        }
        return true;
    }

    auto results() -> const Results&
    {
        static const Results r = run_all();
        return r;
    }
} // namespace

TEST(session, goldens)
{
    const Results& r = results();
    std::vector<Golden> goldens;
    if (!read_goldens(goldens))
    {
        check::fail(__FILE__, __LINE__, "session_golden.txt missing or malformed (dw_unit --record-session)");
        return;
    }
    CHECK_EQ(goldens.size(), r.hashes.size());
    for (const Golden& got : r.hashes)
    {
        auto it = std::find_if(goldens.begin(), goldens.end(), [&](const Golden& g) { return g.seed == got.seed; });
        if (it == goldens.end())
        {
            check::fail(__FILE__, __LINE__, "a seed without a golden");
            continue;
        }
        CHECK_EQ(it->frames, got.frames);
        if (it->hash != got.hash)
        {
            check::fail(__FILE__, __LINE__, "session hash differs from session_golden.txt");
            std::printf("    seed %" PRIu64 ": %016" PRIx64 ", golden %016" PRIx64 "\n", got.seed, got.hash, it->hash);
        }
    }
}

TEST(session, coverage)
{
    CHECK_EQ(check_coverage(results().total), 0);
}

// dw_unit --record-session (main.cpp): runs the sessions and, if coverage holds, rewrites session_golden.txt.
auto record_session() -> int
{
    const Results& r = results();
    if (check_coverage(r.total) != 0)
    {
        std::printf("coverage incomplete: %s not written\n", golden_path().c_str());
        return 1;
    }
    std::ofstream file(golden_path());
    file << "# seed frames fnv1a64: tests/unit/session_test.cpp, MSVC only; re-record with dw_unit --record-session\n";
    for (const Golden& g : r.hashes)
    {
        char line[64];
        std::snprintf(line, sizeof(line), "%" PRIu64 " %" PRIu64 " %016" PRIx64 "\n", g.seed, g.frames, g.hash);
        file << line;
    }
    file.close();
    if (!file)
    {
        std::printf("cannot write %s\n", golden_path().c_str());
        return 1;
    }
    std::printf("recorded %s\n", golden_path().c_str());
    return 0;
}
