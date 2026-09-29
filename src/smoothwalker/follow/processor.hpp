// The follow as the camera core's view processor (camera/api.hpp, docs/design.md "Core and processors"): its switch
// and toggle generation, its settings and their generation, and what the follow reads besides the core's frame
// (mode writes, the aiming / combat / traversal flags), all on Smoothwalker's side of the table. No Unreal or UE4SS
// types: the core calls frame() and state() on the hook thread. Owned by the Smoothwalker component
// (smoothwalker/smoothwalker.cpp) and registered with the core for its whole life.
#pragma once

#include "../../camera/api.hpp"
#include "../settings/settings.hpp"
#include "follow.hpp"

#include <atomic>
#include <cstdint>

namespace dwsw
{
    inline auto follow_tuning_of(const dwsc::Settings& s) -> FollowTuning
    {
        FollowTuning t;
        t.follow_rate_h = s.follow_rate_h;
        t.follow_rate_v = s.follow_rate_v;
        t.curve_h = s.curve_h;
        t.curve_v = s.curve_v;
        t.catchup_distance = s.catchup_distance;
        t.min_rate_scale = s.min_rate_scale;
        t.max_lag_h = s.max_lag_h;
        t.max_lag_v = s.max_lag_v;
        t.soft_leash = s.soft_leash;
        t.rotation_smoothing = s.rotation_smoothing;
        t.wall_clamp = s.wall_clamp;
        t.rotation_rate = s.rotation_rate;
        t.aiming_keep = s.aiming_follow / 100.0;
        t.combat_follow_keep = s.combat_follow / 100.0;
        t.combat_rotation_keep = s.combat_rotation / 100.0;
        t.traversal_follow_keep = s.traversal_follow / 100.0;
        t.traversal_rotation_keep = s.traversal_rotation / 100.0;
        t.transition = s.position_transition;
        return t;
    }

    class FollowProcessor
    {
      public:
        FollowProcessor() = default;
        FollowProcessor(const FollowProcessor&) = delete;
        auto operator=(const FollowProcessor&) -> FollowProcessor& = delete;

        // What Api::register_processor takes; valid for this object's life.
        auto registration() const -> const dwcam::Processor* { return &m_registration; }

        // A publish (under Smoothwalker's file mutex). Only a changed value bumps the generation: the core
        // crossfades on it, and a fade the switches or a shoulder swap started would hold part of the old lag for the
        // transition time.
        auto publish(const FollowTuning& t) -> void
        {
            AcquireSRWLockExclusive(&m_lock);
            if (!same_values(t, m_tuning))
            {
                m_tuning = t;
                ++m_settings_generation;
            }
            ReleaseSRWLockExclusive(&m_lock);
        }

        // The settings as published, for the debug overlay. Any thread but the hook.
        auto tuning() -> FollowTuning
        {
            AcquireSRWLockShared(&m_lock);
            const FollowTuning t = m_tuning;
            ReleaseSRWLockShared(&m_lock);
            return t;
        }

        // The live switch (O, the ini, the Mod Menu). The toggle generation goes first: a hook frame between the two
        // must still fade.
        auto set_enabled(bool on) -> void
        {
            if (m_enabled.load() == on) return;
            m_toggle_generation.fetch_add(1);
            m_enabled.store(on);
        }
        // Startup only: the switch as the file has it, no fade.
        auto store_enabled(bool on) -> void { m_enabled.store(on); }
        auto enabled() const -> bool { return m_enabled.load(); }

        // log_stats: count following frames and their shown lag for the report.
        auto set_log_stats(bool on) -> void { m_log_stats.store(on); }
        auto log_stats() const -> bool { return m_log_stats.load(); }

        // Game thread, each engine tick: which camera-mode groups are blending in or active. The combat and
        // traversal setters return the previous value, for the verbose log.
        auto set_aiming(bool on) -> void { m_aiming.store(on); }
        auto swap_combat(bool on) -> bool { return m_combat.exchange(on); }
        auto swap_traversal(bool on) -> bool { return m_traversal.exchange(on); }

        // Game thread: a camera-mode write landed; its FOV shows on the next camera update, so it crossfades, and its
        // distance glides in, so the wall clamp holds off meanwhile.
        auto mode_written() -> void { m_position_generation.fetch_add(1); }

        struct Stats
        {
            uint64_t frames = 0, clamped = 0;
            double lag_sum = 0.0;
        };
        // The log_stats report: since the last take, then zeroed.
        auto take_stats() -> Stats
        {
            Stats s;
            s.frames = m_frames.exchange(0);
            s.clamped = m_clamped.exchange(0);
            s.lag_sum = m_lag_sum.exchange(0.0);
            return s;
        }

      private:
        SRWLOCK m_lock = SRWLOCK_INIT; // m_tuning, m_settings_generation
        FollowTuning m_tuning = follow_tuning_of(dwsc::Settings{});
        uint64_t m_settings_generation = 0;

        std::atomic<bool> m_enabled{true};
        std::atomic<uint64_t> m_toggle_generation{0};
        std::atomic<uint64_t> m_position_generation{0}; // mode writes; their FOV lands on the next camera update
        std::atomic<bool> m_aiming{false};    // an aiming camera mode is blending in or active
        std::atomic<bool> m_combat{false};    // a combat camera mode is blending in or active
        std::atomic<bool> m_traversal{false}; // a traversal camera mode is blending in or active
        std::atomic<bool> m_log_stats{false};
        std::atomic<uint64_t> m_frames{0};
        std::atomic<uint64_t> m_clamped{0};
        std::atomic<double> m_lag_sum{0.0};

        // Hook thread only: calls arrive in sequence.
        Follow m_follow;
        uint64_t m_seen_settings = 0, m_seen_position = 0;
        uint64_t m_generation = 0; // FrameOut::generation: bumped by a settings change or a mode write

        // Hook thread, once per player-camera update that reaches the core's pipeline.
        static auto frame(void* user, const dwcam::FrameIn* in, dwcam::FrameOut* out) -> void
        {
            auto& self = *static_cast<FollowProcessor*>(user);
            AcquireSRWLockShared(&self.m_lock);
            const FollowTuning t = self.m_tuning;
            const uint64_t settings = self.m_settings_generation;
            ReleaseSRWLockShared(&self.m_lock);

            FollowInputs sw;
            const auto position = self.m_position_generation.load(std::memory_order_relaxed);
            sw.mode_write = position != self.m_seen_position;
            self.m_seen_position = position;
            sw.aiming = self.m_aiming.load(std::memory_order_relaxed);
            sw.combat = self.m_combat.load(std::memory_order_relaxed);
            sw.traversal = self.m_traversal.load(std::memory_order_relaxed);
            if (settings != self.m_seen_settings || sw.mode_write) ++self.m_generation;
            self.m_seen_settings = settings;

            FollowReport report;
            self.m_follow.frame(t, sw, *in, *out, report);
            out->generation = self.m_generation;
            out->transition = t.transition;

            if (report.clamped) self.m_clamped.fetch_add(1, std::memory_order_relaxed);
            if (report.stats && self.m_log_stats.load(std::memory_order_relaxed))
            {
                self.m_frames.fetch_add(1, std::memory_order_relaxed);
                self.m_lag_sum.store(self.m_lag_sum.load(std::memory_order_relaxed) + report.shown_lag, std::memory_order_relaxed);
            }
        }

        // Any thread, lock-free.
        static auto state(void* user, uint64_t* toggle_generation) -> int32_t
        {
            auto& self = *static_cast<FollowProcessor*>(user);
            const bool on = self.m_enabled.load(std::memory_order_relaxed);
            if (toggle_generation) *toggle_generation = self.m_toggle_generation.load(std::memory_order_relaxed);
            return on ? 1 : 0;
        }

        const dwcam::Processor m_registration{sizeof(dwcam::Processor), this, &FollowProcessor::frame, &FollowProcessor::state};
    };

    // The part of a publish the core's hook reads: the follow's settings, the cut thresholds (smoothwalker.ini's,
    // pushed to the core on every publish) and the log switches. Under Smoothwalker's file mutex.
    inline auto publish_view(FollowProcessor& processor, const dwcam::Api& core, const dwsc::Settings& s) -> void
    {
        processor.publish(follow_tuning_of(s));
        core.set_cut_thresholds(s.reset_distance, s.reset_gap);
        processor.set_log_stats(s.log_stats);
        dwsc::g_log_verbose.store(s.log_verbose);
        core.set_diagnostics((s.log_verbose ? dwcam::DIAG_VERBOSE : 0u) | (s.log_stats ? dwcam::DIAG_HOOK_TIMING : 0u));
    }
} // namespace dwsw
