// The API view snapshot (docs/design.md, "Slice 1 as built"): what the hook handed back on the player's last camera
// update, published once per update under a seqlock. One writer (the hook, one player-camera update at a time), any
// number of readers on any thread (Smoothwalker.view() and live(), claim()'s fade check, CameraCore::read_view).
// Numbers only. Needs the Windows types included before it (camera/clock.hpp).
#pragma once

#include "api.hpp"
#include "clock.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dw::camera
{
    struct Snapshot
    {
        View game;        // the view as the game built it, before Smoothwalker
        View shown;       // what Smoothwalker handed back (equal to game when off)
        double pivot[3];  // the character pivot the follow is built around; NAN when unknown
        double half_height; // the capsule's half height under that pivot, cm; NAN when unknown
        double follow_offset[3]; // the core's out_offset (ViewFeed::follow_offset); NAN with the pivot
        double follow_yaw;       // the shown yaw before other mods' layers, degrees; NAN with the pivot
        int64_t qpc;      // the publisher's clock (QueryPerformanceCounter) at publish
    };

    // s since the snapshot was stamped, on `clock`, which counts the same ticks as the stamping clock: one now().
    inline auto age_seconds(const Snapshot& s, const Clock& clock) -> double
    {
        return static_cast<double>(clock.now() - s.qpc) / clock.frequency();
    }

    class ViewSnapshot
    {
      public:
        // The hook. `qpc`: the stamp, the publisher's clock now.
        // `follow_offset` and `follow_yaw` are taken only with a pivot: NAN without one.
        auto publish(const View& game, const View& shown, const double* pivot, double half_height, const double* follow_offset,
                     double follow_yaw, int64_t qpc) -> void
        {
            Snapshot s{};
            s.game = game;
            s.shown = shown;
            for (int i = 0; i < 3; ++i) s.pivot[i] = pivot ? pivot[i] : NAN;
            s.half_height = half_height;
            for (int i = 0; i < 3; ++i) s.follow_offset[i] = pivot && follow_offset ? follow_offset[i] : NAN;
            s.follow_yaw = pivot ? follow_yaw : NAN;
            s.qpc = qpc;
            auto v = m_seq.load(std::memory_order_relaxed);
            m_seq.store(v + 1, std::memory_order_release);
            std::memcpy(&m_data, &s, sizeof(s));
            m_seq.store(v + 2, std::memory_order_release);
        }

        // Any thread. False when nothing was published yet or a write was in flight on every try.
        auto read(Snapshot& out) const -> bool
        {
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                auto a = m_seq.load(std::memory_order_acquire);
                if (a == 0 || (a & 1)) continue;
                std::memcpy(&out, &m_data, sizeof(out));
                std::atomic_thread_fence(std::memory_order_acquire);
                if (m_seq.load(std::memory_order_relaxed) == a) return true;
            }
            return false;
        }

        // Read-only, no seqlock: for a single-threaded check of the published state (tests/unit/session_test.cpp).
        auto seq() const -> uint64_t { return m_seq.load(); }
        auto data() const -> const Snapshot& { return m_data; }

      private:
        std::atomic<uint64_t> m_seq{0};
        Snapshot m_data{};
    };
} // namespace dw::camera
