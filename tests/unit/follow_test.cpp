// smoothwalker/follow: the leash, the restart, the crouch hold, the rotation trail cap, the aiming / combat /
// traversal shares and Influence, the mode-write hold of the wall clamp, and the processor's generation.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "check.hpp"
#include "smoothwalker/follow/follow.hpp"
#include "smoothwalker/follow/processor.hpp"

#include <algorithm>
#include <cmath>

using namespace dw::smoothwalker::follow;
using dw::camera::FrameIn;
using dw::camera::FrameOut;

namespace
{
    // Balanced, the shipped defaults.
    auto balanced() -> FollowTuning { return follow_tuning_of(dw::smoothwalker::settings::Settings{}); }

    // Standing at `pivot` (the capsule centre), the game's camera 300 cm behind and 80 up, level.
    auto frame_at(dw::Vec3 pivot, double dt = 1.0 / 60.0, bool restart = false) -> FrameIn
    {
        FrameIn in{};
        in.enabled = true;
        in.restart = restart;
        in.dt = dt;
        in.pivot = pivot;
        in.half_height = 96.0;
        in.camera = pivot + dw::Vec3{-300, 0, 80};
        in.rotation = dw::from_rotator(0, 0, 0);
        return in;
    }

    struct Run
    {
        Follow follow;
        FollowTuning tuning = balanced();
        FollowInputs inputs;
        FrameOut out{};
        FollowReport report;

        auto step(const FrameIn& in) -> const FrameOut&
        {
            out = FrameOut{};
            follow.frame(tuning, inputs, in, out, report);
            inputs.mode_write = false;
            return out;
        }
    };
} // namespace

TEST(follow, leash)
{
    CHECK_EQ(leash(30.0, 85.0, false), 30.0);
    CHECK_EQ(leash(300.0, 85.0, false), 85.0);
    CHECK_EQ(leash(300.0, 85.0, true), 85.0 * std::tanh(300.0 / 85.0));
    CHECK(leash(1e9, 85.0, true) <= 85.0);
    CHECK_EQ(leash(50.0, 0.0, true), 0.0);
    CHECK_EQ(leash(50.0, -5.0, false), 0.0);
}

TEST(follow, restart_shows_the_game_view)
{
    Run r;
    const FrameIn in = frame_at({100, 200, 96}, 1.0 / 60.0, true);
    r.step(in);
    CHECK_EQ(r.out.location.x, in.camera.x);
    CHECK_EQ(r.out.location.z, in.camera.z);
    CHECK(!r.out.rotated);
    CHECK(!r.out.feed);
    CHECK(!r.report.stats);
    CHECK_EQ(r.out.nominal_distance, dw::length(in.camera - in.pivot));
}

TEST(follow, off_passes_through)
{
    Run r;
    FrameIn in = frame_at({0, 0, 96}, 1.0 / 60.0, true);
    r.step(in);
    in = frame_at({50, 0, 96});
    in.enabled = false;
    r.step(in);
    CHECK_EQ(r.out.location.x, in.camera.x);
    CHECK(!r.out.feed);
    CHECK(!r.report.stats);
}

TEST(follow, hard_leash_caps_the_trail)
{
    Run r;
    r.tuning.soft_leash = false;
    r.tuning.follow_rate_h = 0.5;
    r.step(frame_at({0, 0, 96}, 1.0 / 60.0, true));
    r.step(frame_at({1000, 0, 96}));
    CHECK_NEAR(r.report.shown_lag, 85.0, 1e-9);
    CHECK_NEAR(r.out.location.x, 1000.0 - 85.0 - 300.0, 1e-9);
}

TEST(follow, soft_leash_eases_into_the_limit)
{
    Run r;
    r.tuning.follow_rate_h = 0.5;
    r.step(frame_at({0, 0, 96}, 1.0 / 60.0, true));
    r.step(frame_at({1000, 0, 96}));
    // Internal lag held at 3x the limit, shown through tanh.
    CHECK_NEAR(r.report.shown_lag, 85.0 * std::tanh(3.0), 1e-9);
}

TEST(follow, catches_up)
{
    Run r;
    r.step(frame_at({0, 0, 96}, 1.0 / 60.0, true));
    r.step(frame_at({60, 0, 96}));
    CHECK(r.report.shown_lag > 0.0);
    for (int i = 0; i < 600; ++i) r.step(frame_at({60, 0, 96}));
    CHECK_NEAR(r.out.location.x, 60.0 - 300.0, 1e-3);
    CHECK_NEAR(r.report.shown_lag, 0.0, 1e-3);
}

// docs/design.md "Crouch hold": the capsule shrinks from half height 96 to 42 and the centre drops 54 in one frame,
// the feet stay; the game eases its camera down the same 54 over about a third of a second. The change frame shows
// the game's camera exactly, the hold then keeps the camera above the game's while that eases, never more than the
// vertical leash, and lets go; crouch_drop stops tracking 0.6 s in.
namespace
{
    // The game's camera height over the change, feet-relative, as traced: still for 130 ms, about 90 % down by 290 ms
    // (a smoothstep down 54 cm over 220 ms).
    auto game_drop(double t) -> double
    {
        double s = std::clamp((t - 0.13) / 0.22, 0.0, 1.0);
        return 54.0 * s * s * (3.0 - 2.0 * s);
    }

    struct Crouch
    {
        double peak = 0.0, at_change = NAN, after_3s = NAN;
        double z_at_1s = NAN;
    };

    auto crouch(double pitch_shift_at_0_7) -> Crouch
    {
        Run r;
        const double dt = 1.0 / 64.0;
        auto at = [&](double t, double half) {
            FrameIn in = frame_at({0, 0, half}, dt); // feet at 0
            in.half_height = half;
            in.camera.z = 176.0 - game_drop(t) + (t >= 0.7 ? pitch_shift_at_0_7 : 0.0);
            return in;
        };
        FrameIn in = at(-1.0, 96.0);
        in.restart = true;
        r.step(in);
        for (int i = 0; i < 32; ++i) r.step(at(-1.0, 96.0));
        Crouch c;
        for (int i = 0; i <= 3 * 64; ++i)
        {
            double t = i * dt;
            in = at(t, 42.0);
            r.step(in);
            double above = r.out.location.z - in.camera.z;
            if (i == 0) c.at_change = above;
            c.peak = std::max(c.peak, above);
            if (i == 64) c.z_at_1s = r.out.location.z;
        }
        c.after_3s = r.out.location.z - in.camera.z;
        return c;
    }
} // namespace

TEST(follow, crouch_hold)
{
    Crouch c = crouch(0.0);
    CHECK_NEAR(c.at_change, 0.0, 1e-9); // the change frame: the game's camera, no pop
    CHECK_NEAR(c.peak, 21.0, 1.0);      // held above the game's easing: design.md traced a 21 cm peak
    CHECK(c.peak < 50.0);               // inside the vertical leash (max_lag_v 50)
    CHECK_NEAR(c.after_3s, 0.0, 0.1);   // let go
}

TEST(follow, crouch_drop_stops_at_0_6_s)
{
    // A pitch change after 0.6 s is not taken for more crouch: it passes straight through.
    Crouch still = crouch(0.0), pitched = crouch(30.0);
    CHECK_NEAR(pitched.z_at_1s - still.z_at_1s, 30.0, 1e-9);
}

TEST(follow, rotation_trail_capped_at_90_degrees)
{
    Run r;
    r.tuning.rotation_smoothing = true;
    r.tuning.rotation_rate = 1.0; // the slowest turning follow
    FrameIn in = frame_at({0, 0, 96}, 1.0 / 60.0, true);
    r.step(in);
    double worst = 0.0;
    for (int i = 1; i <= 72; ++i) // two full turns, 20 degrees per frame
    {
        in = frame_at({0, 0, 96});
        in.rotation = dw::from_rotator(0, 20.0 * i, 0);
        r.step(in);
        CHECK(r.out.rotated);
        worst = std::max(worst, dw::angle_between(r.out.rotation, in.rotation));
    }
    CHECK(worst <= 0.5 * dw::PI + 1e-9);
    CHECK(worst > 0.5 * dw::PI - 1e-3); // a fast spin rides the cap
}

TEST(follow, influence_and_keep_shares)
{
    struct Case
    {
        bool aiming, combat, traversal;
        Influence expect;
        double keep;
    };
    FollowTuning t = balanced();
    t.aiming_keep = 0.3;
    t.combat_follow_keep = 0.6;
    t.traversal_follow_keep = 0.8;
    const Case cases[] = {
            {false, false, false, Influence::None, 1.0},  {true, false, false, Influence::Aiming, 0.3},
            {false, true, false, Influence::Combat, 0.6}, {false, false, true, Influence::Traversal, 0.8},
            {true, false, true, Influence::Aiming, 0.3},  {false, true, true, Influence::Combat, 0.6},
    };
    for (const Case& c : cases)
    {
        Run r;
        r.tuning = t;
        r.step(frame_at({0, 0, 96}, 1.0 / 60.0, true));
        r.inputs.aiming = c.aiming;
        r.inputs.combat = c.combat;
        r.inputs.traversal = c.traversal;
        for (int i = 0; i < 300; ++i) r.step(frame_at({0, 0, 96}));
        CHECK(r.out.feed);
        CHECK_EQ(r.out.influence, static_cast<int>(c.expect));
        CHECK_NEAR(r.out.keep_follow, c.keep, 1e-6);
    }
}

TEST(follow, restart_snaps_the_shares)
{
    Run r;
    r.tuning.aiming_keep = 0.3;
    r.inputs.aiming = true;
    r.step(frame_at({0, 0, 96}, 1.0 / 60.0, true));
    r.step(frame_at({0, 0, 96}));
    CHECK_NEAR(r.out.keep_follow, 0.3, 1e-12); // aiming at full weight from the restart on, no ease
    CHECK_EQ(r.out.influence, static_cast<int>(Influence::Aiming));
}

// docs/design.md "Walls": for position_transition + 0.3 s after a mode write the recent distance tracks the game's,
// so a shorter distance gliding in is not taken for a wall. transition 0.2: a 0.5 s hold, two 0.25 s steps.
TEST(follow, mode_write_holds_the_wall_clamp)
{
    Run r;
    r.tuning.transition = 0.2;
    auto at = [](double arm) {
        FrameIn in = frame_at({0, 0, 96}, 0.25);
        in.camera = in.pivot + dw::Vec3{-arm, 0, 0};
        return in;
    };
    FrameIn in = at(300);
    in.restart = true;
    r.step(in);
    r.inputs.mode_write = true;
    r.step(at(300));
    CHECK_EQ(r.out.nominal_distance, 300.0);
    r.step(at(200)); // still held: tracks the game at once
    CHECK_EQ(r.out.nominal_distance, 200.0);
    r.step(at(120)); // the hold ran out: the recent distance settles at 1/s and stays above the game's
    CHECK(r.out.nominal_distance > 120.0);
    CHECK_NEAR(r.out.nominal_distance, 200.0 + (120.0 - 200.0) * (1.0 - std::exp(-0.25)), 1e-9);
}

TEST(follow, wall_clamp_without_a_mode_write)
{
    Run r;
    r.tuning.follow_rate_h = 0.5;
    auto at = [](double x, double arm) {
        FrameIn in = frame_at({x, 0, 96});
        in.camera = in.pivot + dw::Vec3{-arm, 0, 0};
        return in;
    };
    FrameIn in = at(0, 300);
    in.restart = true;
    r.step(in);
    r.step(at(60, 300)); // moving away from the camera puts the lag along the arm, farther than the game's camera
    CHECK(!r.report.clamped);
    CHECK(dw::length(r.out.location - dw::Vec3{60, 0, 96}) > 300.0);
    r.step(at(60, 150)); // the game pulled its camera in to half: clamped to the game's distance
    CHECK(r.report.clamped);
    CHECK_NEAR(dw::length(r.out.location - dw::Vec3{60, 0, 96}), 150.0, 1e-9);
}

TEST(follow, processor_generation)
{
    FollowProcessor p;
    dw::camera::Processor& core_side = p;
    FrameIn in = frame_at({0, 0, 96}, 1.0 / 60.0, true);
    FrameOut out{};
    core_side.frame(in, out);
    const uint64_t g0 = out.generation;
    CHECK_EQ(out.transition, 0.5);

    in.restart = false;
    out = FrameOut{};
    core_side.frame(in, out);
    CHECK_EQ(out.generation, g0); // nothing changed

    p.publish(balanced()); // the same values: no generation
    out = FrameOut{};
    core_side.frame(in, out);
    CHECK_EQ(out.generation, g0);

    p.mode_written(); // a mode write crossfades
    out = FrameOut{};
    core_side.frame(in, out);
    CHECK_EQ(out.generation, g0 + 1);

    FollowTuning t = balanced();
    t.max_lag_h = 40.0;
    p.publish(t); // a changed value crossfades
    out = FrameOut{};
    core_side.frame(in, out);
    CHECK_EQ(out.generation, g0 + 2);
}

TEST(follow, processor_switch)
{
    FollowProcessor p;
    const dw::camera::Processor& core_side = p;
    uint64_t toggle = 99;
    CHECK(core_side.state(toggle));
    CHECK_EQ(toggle, 0u);
    p.set_enabled(true); // no change: no generation
    core_side.state(toggle);
    CHECK_EQ(toggle, 0u);
    p.set_enabled(false);
    CHECK(!core_side.state(toggle));
    CHECK_EQ(toggle, 1u);
    p.store_enabled(true); // startup: no fade
    CHECK(core_side.state(toggle));
    CHECK_EQ(toggle, 1u);
}
