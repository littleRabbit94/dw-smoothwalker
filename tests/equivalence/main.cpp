// The equivalence harness's session driver: one seeded random session per run, applied event by event to the
// baseline and the split build in lockstep. Before every event both builds get the same clock; after it, their
// records (the view bytes the hook left, everything the hook publishes, the API's state, the stats counters, the
// debug overlay's reads) and their clocks must be identical to the byte, or the run stops at the first difference.
//
//   equivalence <seed> <frames>

#include "harness.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>

namespace harness
{
    namespace
    {
        int64_t g_clock = 1'000'000'000; // QPC ticks, 10 MHz
        int g_fault_at = 0;              // the guarded access to fail in the current event, counted from 1; 0: none
        int g_fault_count = 0;
        alignas(16) uint8_t g_game_view[96]{}; // what the game's GetCameraView writes this frame
    } // namespace

    auto qpc_tick() -> int64_t
    {
        return g_clock++;
    }

    auto fault_now() -> bool
    {
        return ++g_fault_count == g_fault_at;
    }

    auto original_view(void*, float, void* view) -> void
    {
        memcpy(view, g_game_view, 64); // FMinimalViewInfo's head and more; the rest of the buffer is the caller's
    }
} // namespace harness

namespace
{
    using harness::Record;
    using harness::Variant;

    constexpr double FREQ = 1e7;
    constexpr size_t VIEW_BUFFER = 96;

    struct Counters
    {
        uint64_t events = 0, hook_calls = 0, player_updates = 0, other_calls = 0, publishes = 0, toggles = 0, mode_writes = 0,
                 cuts_requested = 0, claims_ok = 0, claims_refused = 0, renewals = 0, releases_glide = 0, releases_cut = 0, uninstalls = 0,
                 rekeys = 0, layer_sets = 0, layer_refused = 0, layer_no_slot = 0, layer_clears = 0, stats_takes = 0, panels = 0, faults = 0, nan_views = 0,
                 pauses = 0, teleports = 0, crouches = 0, bad_half_heights = 0, crossfades = 0, glides = 0, transition_zero_publishes = 0;
        std::map<int, uint64_t> snaps; // dwsc::Snap -> restarts seen
    };

    const char* const SNAP_NAMES[] = {"None", "Startup", "Teleport", "Gap", "World", "Player", "Pawn", "Toggle", "ApiCut", "ClaimEnded", "ViewLost"};

    class Session
    {
      public:
        Session(uint64_t seed) : m_rng(seed), m_base(harness::make_base()), m_new(harness::make_new())
        {
            memset(m_root, 0, sizeof(m_root));
            m_settings = random_settings(harness::HSettings{});
            both("init", [&](Variant& v, Record&) { v.init(m_settings, m_enabled); });
            place_player(true);
        }

        auto set_rekey_chance(double p) -> void { m_rekey_chance = p; }

        auto run(uint64_t frames) -> bool
        {
            for (m_frame = 0; m_frame < frames; ++m_frame)
            {
                if (!frame()) return false;
            }
            return true;
        }

        auto print(uint64_t seed, uint64_t frames) const -> void
        {
            const auto& c = m_counters;
            std::printf("seed %" PRIu64 ": %" PRIu64 " frames, %" PRIu64 " events, 0 mismatches\n", seed, frames, c.events);
            std::printf("  hook calls %" PRIu64 " (player %" PRIu64 ", other cameras %" PRIu64 "), NaN views %" PRIu64 ", faults %" PRIu64
                        ", pauses %" PRIu64 ", teleports %" PRIu64 ", crouches %" PRIu64 ", bad half heights %" PRIu64 "\n",
                        c.hook_calls, c.player_updates, c.other_calls, c.nan_views, c.faults, c.pauses, c.teleports, c.crouches, c.bad_half_heights);
            std::printf("  publishes %" PRIu64 " (transition 0: %" PRIu64 "), toggles %" PRIu64 ", mode writes %" PRIu64 ", cuts requested %" PRIu64
                        ", crossfades %" PRIu64 " (glides %" PRIu64 ")\n",
                        c.publishes, c.transition_zero_publishes, c.toggles, c.mode_writes, c.cuts_requested, c.crossfades, c.glides);
            std::printf("  claims %" PRIu64 " (refused %" PRIu64 ", renewals %" PRIu64 "), releases glide %" PRIu64 " / cut %" PRIu64
                        ", uninstalls %" PRIu64 ", re-keys %" PRIu64 ", layer sets %" PRIu64 " (not finite %" PRIu64 ", no slot %" PRIu64 "), clears %" PRIu64
                        ", stats takes %" PRIu64 ", panel reads %" PRIu64 "\n",
                        c.claims_ok, c.claims_refused, c.renewals, c.releases_glide, c.releases_cut, c.uninstalls, c.rekeys, c.layer_sets,
                        c.layer_refused, c.layer_no_slot, c.layer_clears, c.stats_takes, c.panels);
            std::printf("  restarts by reason:");
            for (auto& [snap, n] : c.snaps) std::printf(" %s %" PRIu64, snap >= 0 && snap <= 10 ? SNAP_NAMES[snap] : "?", n);
            std::printf("\n");
        }

      private:
        std::mt19937_64 m_rng;
        std::unique_ptr<Variant> m_base, m_new;
        Record m_rb, m_rn;
        Counters m_counters;
        uint64_t m_frame = 0;
        int64_t m_last_snap_qpc = 0;
        bool m_last_blending = false;

        // The world.
        harness::HSettings m_settings;
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
        double m_rekey_chance = 0.00001; // per consumer and event; the optional third argument raises it
        alignas(16) uint8_t m_root[0x800];
        uint8_t m_camera_object = 0, m_other_object = 0;
        static constexpr int32_t TRANSLATION = 0x200, HALF = 0x1F0;

        auto uniform(double a, double b) -> double { return std::uniform_real_distribution<double>(a, b)(m_rng); }
        auto chance(double p) -> bool { return uniform(0.0, 1.0) < p; }
        auto pick(int n) -> int { return std::uniform_int_distribution<int>(0, n - 1)(m_rng); }

        // Applies one event to both builds from the same clock and compares what they hold after it.
        template <typename Apply>
        auto both(const char* what, Apply&& apply, int fault_at = 0) -> bool
        {
            const int64_t t0 = harness::g_clock;
            m_rb.clear();
            m_rn.clear();
            harness::g_clock = t0;
            harness::g_fault_at = fault_at;
            harness::g_fault_count = 0;
            apply(*m_base, m_rb);
            m_base->state(m_rb);
            const int64_t tb = harness::g_clock;
            harness::g_clock = t0;
            harness::g_fault_count = 0;
            apply(*m_new, m_rn);
            m_new->state(m_rn);
            const int64_t tn = harness::g_clock;
            harness::g_fault_at = 0;
            ++m_counters.events;
            if (tb != tn)
            {
                std::printf("MISMATCH frame %" PRIu64 " event %s: clock %" PRId64 " vs %" PRId64 " (a different number of QPC calls)\n", m_frame, what,
                            tb - t0, tn - t0);
                return false;
            }
            if (m_rb.bytes != m_rn.bytes)
            {
                report(what);
                return false;
            }
            return true;
        }

        auto report(const char* what) const -> void
        {
            std::printf("MISMATCH frame %" PRIu64 " event %s\n", m_frame, what);
            if (m_rb.labels.size() != m_rn.labels.size()) std::printf("  record layouts differ (%zu vs %zu fields)\n", m_rb.labels.size(), m_rn.labels.size());
            size_t n = std::min(m_rb.labels.size(), m_rn.labels.size());
            int shown = 0;
            for (size_t i = 0; i < n && shown < 12; ++i)
            {
                size_t b0 = m_rb.labels[i].second, n0 = m_rn.labels[i].second;
                size_t b1 = i + 1 < m_rb.labels.size() ? m_rb.labels[i + 1].second : m_rb.bytes.size();
                size_t n1 = i + 1 < m_rn.labels.size() ? m_rn.labels[i + 1].second : m_rn.bytes.size();
                bool same = std::strcmp(m_rb.labels[i].first, m_rn.labels[i].first) == 0 && b1 - b0 == n1 - n0 &&
                            std::equal(m_rb.bytes.begin() + b0, m_rb.bytes.begin() + b1, m_rn.bytes.begin() + n0);
                if (same) continue;
                ++shown;
                std::printf("  %s / %s:", m_rb.labels[i].first, m_rn.labels[i].first);
                auto dump = [](const std::vector<uint8_t>& bytes, size_t a, size_t b) {
                    if (b - a == 8)
                    {
                        double d;
                        memcpy(&d, &bytes[a], 8);
                        std::printf(" %.17g", d);
                    }
                    else
                    {
                        for (size_t k = a; k < b && k < a + 64; ++k) std::printf("%02x", bytes[k]);
                    }
                };
                std::printf(" base");
                dump(m_rb.bytes, b0, b1);
                std::printf(" | new");
                dump(m_rn.bytes, n0, n1);
                std::printf("\n");
            }
        }

        auto random_settings(harness::HSettings s) -> harness::HSettings
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

        auto place_player(bool present) -> bool
        {
            void* camera = present && m_camera_gone == 0 ? &m_camera_object : nullptr;
            void* root = camera && m_root_gone == 0 ? m_root : nullptr;
            int32_t translation = m_offset_gone > 0 ? -1 : TRANSLATION;
            int32_t half = m_half_offset_missing ? -1 : HALF;
            return both("set_player", [&](Variant& v, Record&) { v.set_player(camera, root, translation, half); });
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
                else if (which == 2) view[pick(3)] = INFINITY;
                else fov = NAN;
            }
            for (size_t i = 0; i < sizeof(harness::g_game_view); ++i) harness::g_game_view[i] = static_cast<uint8_t>(0xA5 ^ i);
            memcpy(harness::g_game_view, view, 48);
            memcpy(harness::g_game_view + 48, &fov, 4);
        }

        auto write_root() -> void
        {
            double half = m_half;
            if (m_bad_half_frames > 0)
            {
                static const double bad[] = {NAN, -5.0, 5000.0, INFINITY, 1000.5};
                half = bad[m_frame % 5];
            }
            memcpy(m_root + TRANSLATION, m_pos, sizeof(m_pos));
            float h = static_cast<float>(half);
            memcpy(m_root + HALF, &h, sizeof(h));
        }

        auto hook_call(void* self, float delta_time, int fault_at) -> bool
        {
            ++m_counters.hook_calls;
            if (self == &m_camera_object) ++m_counters.player_updates;
            else ++m_counters.other_calls;
            alignas(16) uint8_t start[VIEW_BUFFER];
            for (size_t i = 0; i < VIEW_BUFFER; ++i) start[i] = static_cast<uint8_t>(0x3C + i);
            bool ok = both(
                    self == &m_camera_object ? "hook(player)" : "hook(other)",
                    [&](Variant& v, Record& r) {
                        alignas(16) uint8_t view[VIEW_BUFFER];
                        memcpy(view, start, VIEW_BUFFER);
                        v.hook(self, delta_time, view);
                        r.put("view", view, VIEW_BUFFER);
                    },
                    fault_at);
            if (!ok) return false;
            // Coverage, from the baseline's record: a restart, a crossfade starting.
            int64_t snap_qpc;
            int snap;
            size_t at_snap = 0, at_qpc = 0, at_blend = 0, at_glide = 0;
            for (auto& [label, offset] : m_rb.labels)
            {
                if (std::strcmp(label, "debug.snap") == 0) at_snap = offset;
                else if (std::strcmp(label, "debug.snap_qpc") == 0) at_qpc = offset;
                else if (std::strcmp(label, "api.blending") == 0) at_blend = offset;
                else if (std::strcmp(label, "debug.glide") == 0) at_glide = offset;
            }
            memcpy(&snap, &m_rb.bytes[at_snap], sizeof(snap));
            memcpy(&snap_qpc, &m_rb.bytes[at_qpc], sizeof(snap_qpc));
            bool blending = m_rb.bytes[at_blend] != 0;
            bool glide = m_rb.bytes[at_glide] != 0;
            if (snap_qpc != m_last_snap_qpc) ++m_counters.snaps[snap];
            m_last_snap_qpc = snap_qpc;
            if (blending && !m_last_blending) ++m_counters.crossfades, m_counters.glides += glide ? 1 : 0;
            m_last_blending = blending;
            return true;
        }

        auto game_thread_events() -> bool
        {
            // Settings.
            if (chance(0.003))
            {
                ++m_counters.publishes;
                if (!chance(0.25)) m_settings = random_settings(m_settings); // else the same values again: no fade
                if (m_settings.position_transition == 0.0) ++m_counters.transition_zero_publishes;
                if (!both("publish", [&](Variant& v, Record&) { v.publish(m_settings); })) return false;
            }
            if (chance(0.002))
            {
                ++m_counters.toggles;
                m_enabled = !m_enabled;
                if (!both("toggle", [&](Variant& v, Record&) { v.set_enabled(m_enabled); })) return false;
            }
            if (chance(0.0005) && !both("toggle(same)", [&](Variant& v, Record&) { v.set_enabled(m_enabled); })) return false;
            // Camera modes.
            if (chance(0.01)) m_aiming = !m_aiming;
            if (chance(0.006)) m_combat = !m_combat;
            if (chance(0.006)) m_traversal = !m_traversal;
            if (!both("modes", [&](Variant& v, Record& r) { v.set_modes(m_aiming, m_combat, m_traversal, r); })) return false;
            if (chance(0.003))
            {
                ++m_counters.mode_writes;
                if (!both("mode_write", [&](Variant& v, Record&) { v.mode_write(); })) return false;
            }
            // The core's own cuts: a level change, a controller or a pawn.
            if (chance(0.0015))
            {
                ++m_counters.cuts_requested;
                static const int reasons[] = {4, 5, 6}; // World, Player, Pawn
                int reason = reasons[pick(3)];
                if (!both("request_cut", [&](Variant& v, Record&) { v.request_cut(reason); })) return false;
                if (reason != 6 && chance(0.5))
                {
                    // The player forgotten for a while: no updates reach the pipeline.
                    m_camera_gone = 1 + pick(200);
                    if (!place_player(true)) return false;
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
                        if (!both("install", [&](Variant& v, Record&) { v.install(c); })) return false;
                    }
                    continue;
                }
                if (chance(0.003))
                {
                    bool keep = chance(0.5);
                    double ttl = chance(0.5) ? 0.0 : chance(0.02) ? NAN : uniform(0.05, 3.0);
                    int code = 0;
                    if (!both("claim", [&](Variant& v, Record& r) {
                            code = v.claim(c, keep, ttl);
                            r.put("claim", code);
                        }))
                        return false;
                    if (code == harness::Ok) ++m_counters.claims_ok;
                    else if (code == harness::AlreadyYours) ++m_counters.renewals;
                    else ++m_counters.claims_refused;
                }
                if (chance(0.0035))
                {
                    bool glide = chance(0.5);
                    int code = 0;
                    if (!both("release", [&](Variant& v, Record& r) {
                            code = v.release(c, glide);
                            r.put("release", code);
                        }))
                        return false;
                    if (code == harness::Ok) ++(glide ? m_counters.releases_glide : m_counters.releases_cut);
                }
                if (chance(0.0003))
                {
                    ++m_counters.uninstalls;
                    m_installed[c] = false;
                    if (!both("uninstall", [&](Variant& v, Record&) { v.uninstall(c); })) return false;
                    continue;
                }
                // Rare: a re-key without on_lua_stop. It frees the layer slot the consumer held (lua_api.hpp,
                // install_locked); 47f6645 leaked it, so the baseline carries baseline-fixes/0001 to agree.
                if (chance(m_rekey_chance))
                {
                    ++m_counters.rekeys;
                    if (!both("install(re-key)", [&](Variant& v, Record&) { v.install(c); })) return false;
                }
                if (chance(0.004))
                {
                    harness::LayerArgs a;
                    for (int k = 0; k < 3; ++k) a.offset[k] = chance(0.5) ? 0.0 : uniform(-100.0, 100.0);
                    for (int k = 0; k < 3; ++k) a.rotation[k] = chance(0.5) ? 0.0 : uniform(-20.0, 20.0);
                    a.fov = chance(0.5) ? 0.0 : uniform(-20.0, 20.0);
                    a.fov_abs = chance(0.7) ? NAN : uniform(0.0, 200.0);
                    a.weight = uniform(-0.2, 1.2);
                    a.blend = chance(0.3) ? 0.0 : uniform(-0.5, 2.0);
                    a.ttl = chance(0.5) ? 0.0 : uniform(0.05, 3.0);
                    if (chance(0.03)) a.offset[pick(3)] = INFINITY;
                    int code = 0;
                    if (!both("layer_set", [&](Variant& v, Record& r) {
                            code = v.layer_set(c, a);
                            r.put("layer_set", code);
                        }))
                        return false;
                    if (code == harness::Ok) ++m_counters.layer_sets;
                    else if (code == harness::NoSlot) ++m_counters.layer_no_slot;
                    else ++m_counters.layer_refused;
                }
                if (chance(0.002))
                {
                    ++m_counters.layer_clears;
                    if (!both("layer_clear", [&](Variant& v, Record& r) { r.put("layer_clear", v.layer_clear(c)); })) return false;
                }
            }
            return true;
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

        auto frame() -> bool
        {
            // A pause (the camera stops updating), then this frame's time.
            if (chance(0.0015))
            {
                ++m_counters.pauses;
                harness::g_clock += static_cast<int64_t>(uniform(0.1, 5.0) * FREQ);
            }
            double dt = chance(0.01) ? uniform(0.1, 0.4) : uniform(1.0 / 144.0, 1.0 / 30.0);
            harness::g_clock += static_cast<int64_t>(dt * FREQ);
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
                if (!both("request_cut", [&](Variant& v, Record&) { v.request_cut(6); })) return false; // forget_pawn
                replace = true;
            }
            if (m_root_gone == 0 && chance(0.0005)) m_root_gone = 1 + pick(60), replace = true;
            if (m_offset_gone == 0 && chance(0.0003)) m_offset_gone = 1 + pick(60), replace = true;
            if (chance(0.0005)) m_half_offset_missing = !m_half_offset_missing, replace = true;
            if (replace && !place_player(true)) return false;

            if (!game_thread_events()) return false;

            // The camera updates: other cameras around the player's.
            write_root();
            bool nan_view = chance(0.002);
            if (nan_view) ++m_counters.nan_views;
            build_view(nan_view);
            float delta_time = static_cast<float>(dt * m_slowmo);
            if (chance(0.003))
            {
                static const float odd[] = {0.0f, -0.01f, NAN, INFINITY, 1.0f};
                delta_time = odd[pick(5)];
            }
            if (chance(0.3) && !hook_call(&m_other_object, delta_time, 0)) return false;
            int fault = 0;
            if (chance(0.003))
            {
                ++m_counters.faults;
                fault = 1 + pick(4);
            }
            if (!hook_call(&m_camera_object, delta_time, fault)) return false;
            if (chance(0.2) && !hook_call(&m_other_object, delta_time, 0)) return false;

            // The game thread reading back: the log_stats report, the overlay and the keys.
            if (chance(0.01))
            {
                ++m_counters.stats_takes;
                if (!both("take_stats", [&](Variant& v, Record& r) { v.take_stats(r); })) return false;
            }
            if (chance(0.01))
            {
                ++m_counters.panels;
                if (!both("panel", [&](Variant& v, Record& r) { v.panel(r); })) return false;
            }
            return true;
        }
    };
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <seed> <frames> [rekey_chance]\n", argv[0]);
        return 2;
    }
    uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    Session session(seed);
    if (argc > 3) session.set_rekey_chance(std::strtod(argv[3], nullptr));
    if (!session.run(frames))
    {
        std::printf("seed %" PRIu64 ": FAILED\n", seed);
        return 1;
    }
    session.print(seed, frames);
    return 0;
}
