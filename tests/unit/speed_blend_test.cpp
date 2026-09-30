// smoothwalker/follow/speed_blend: the speed share, w from the FOV, one frame's add, the eased speed and the Sprint state
// weight, and the blend through the follow and its processor (the settings it takes, the context overrides, the
// sprint view the game thread hands over, and no change at all while it is off).

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
#include "smoothwalker/follow/speed_blend.hpp"

#include <cmath>
#include <cstring>

using namespace dw::smoothwalker::follow;
using dw::camera::FrameIn;
using dw::camera::FrameOut;
using dw::smoothwalker::settings::Settings;

namespace
{
    constexpr double DT = 1.0 / 60.0;

    // Balanced with the blend on and a Sprint group that differs: 120 % against 100 %, 10 cm higher, 20 cm further out,
    // 6 degrees wider.
    auto blended_settings() -> Settings
    {
        Settings s;
        s.speed_blend = 100;
        s.sprint_distance = 120;
        s.sprint_height = 10;
        s.sprint_shoulder = 20;
        s.sprint_fov = 6;
        return s;
    }

    // At `pivot`, the game's camera 300 cm behind, 50 to the right and 80 up, level, looking along +x.
    auto frame_at(dw::Vec3 pivot, bool restart = false, double fov = 90.0) -> FrameIn
    {
        FrameIn in{};
        in.enabled = true;
        in.restart = restart;
        in.dt = DT;
        in.pivot = pivot;
        in.half_height = 96.0;
        in.camera = pivot + dw::Vec3{-300, 50, 80};
        in.rotation = dw::from_rotator(0, 0, 0);
        in.fov = fov;
        return in;
    }

    struct Run
    {
        Follow follow;
        FollowTuning tuning;
        FollowInputs inputs;
        FrameOut out{};
        FollowReport report;
        dw::Vec3 pivot{0, 0, 96};

        explicit Run(const Settings& s) : tuning(follow_tuning_of(s)) {}

        // `frames` updates walking along +x at `speed` cm/s, the first a restart.
        auto walk(double speed, int frames, bool restart = true) -> const FrameOut&
        {
            for (int i = 0; i < frames; ++i)
            {
                if (i > 0 || !restart) pivot.x += speed * DT;
                out = FrameOut{};
                follow.frame(tuning, inputs, frame_at(pivot, restart && i == 0), out, report);
            }
            return out;
        }
    };

    auto same_bits(const dw::Vec3& a, const dw::Vec3& b) -> bool { return std::memcmp(&a, &b, sizeof(a)) == 0; }
} // namespace

TEST(speed_blend, speed_share_edges)
{
    CHECK_EQ(speed_share(0.0, 150.0, 558.0), 0.0);
    CHECK_EQ(speed_share(131.0, 150.0, 558.0), 0.0); // walk
    CHECK_EQ(speed_share(150.0, 150.0, 558.0), 0.0);
    CHECK_NEAR(speed_share(354.0, 150.0, 558.0), 0.5, 1e-12);
    CHECK_EQ(speed_share(558.0, 150.0, 558.0), 1.0); // sprint
    CHECK_EQ(speed_share(897.0, 150.0, 558.0), 1.0); // haste
    const double run = speed_share(460.0, 150.0, 558.0);
    CHECK(run > 0.75 && run < 0.9); // about 85 %: running sits most of the way there
    // A range the wrong way round is a step at start.
    CHECK_EQ(speed_share(199.0, 200.0, 100.0), 0.0);
    CHECK_EQ(speed_share(200.0, 200.0, 100.0), 1.0);
}

TEST(speed_blend, weight_from_the_fov)
{
    CHECK_EQ(sprint_weight_of_fov(90.0, 90.0, 95.0), 0.0);
    CHECK_NEAR(sprint_weight_of_fov(92.5, 90.0, 95.0), 0.5, 1e-12);
    CHECK_EQ(sprint_weight_of_fov(95.0, 90.0, 95.0), 1.0);
    CHECK_EQ(sprint_weight_of_fov(100.0, 90.0, 95.0), 1.0); // clamped: an aiming FOV reads as all Sprint, adding nothing
    CHECK_EQ(sprint_weight_of_fov(80.0, 90.0, 95.0), 0.0);
    CHECK_NEAR(sprint_weight_of_fov(85.0, 90.0, 80.0), 0.5, 1e-12); // a sprint_fov below exploring's
    CHECK(std::isnan(sprint_weight_of_fov(90.0, 90.0, 90.5)));      // within 1 degree: the state instead
    CHECK(std::isnan(sprint_weight_of_fov(NAN, 90.0, 95.0)));
    CHECK(std::isnan(sprint_weight_of_fov(90.0, NAN, 95.0)));
    CHECK(std::isnan(sprint_weight_of_fov(90.0, 90.0, NAN)));
}

TEST(speed_blend, add_terms)
{
    SpeedBlendTuning t;
    t.amount = 1.0;
    t.distance_ratio = 1.2;
    t.height = 10.0;
    t.shoulder = 20.0;
    t.fov = 6.0;
    const dw::Quat level = dw::from_rotator(0, 0, 0);

    // Behind 300 cm: 60 further back along the view, 10 up, 20 out on the right where the camera sits, 6 degrees.
    SpeedBlendAdd a = speed_blend_add(t, 1.0, {-300, 50, 80}, level, 1.0);
    CHECK_NEAR(a.offset.x, -60.0, 1e-9);
    CHECK_NEAR(a.offset.y, 20.0, 1e-9);
    CHECK_NEAR(a.offset.z, 10.0, 1e-9);
    CHECK_EQ(a.fov, 6.0);

    // Half the share, half of everything.
    a = speed_blend_add(t, 0.5, {-300, 50, 80}, level, 1.0);
    CHECK_NEAR(a.offset.x, -30.0, 1e-9);
    CHECK_NEAR(a.offset.y, 10.0, 1e-9);
    CHECK_NEAR(a.offset.z, 5.0, 1e-9);
    CHECK_EQ(a.fov, 3.0);

    // Against a wall: no distance or height, the shoulder and the FOV stay.
    a = speed_blend_add(t, 1.0, {-300, 50, 80}, level, 0.0);
    CHECK_NEAR(a.offset.x, 0.0, 1e-9);
    CHECK_NEAR(a.offset.y, 20.0, 1e-9);
    CHECK_NEAR(a.offset.z, 0.0, 1e-9);
    CHECK_EQ(a.fov, 6.0);

    // The other shoulder, and a centred mode: out on the left, none.
    a = speed_blend_add(t, 1.0, {-300, -50, 80}, level, 1.0);
    CHECK_NEAR(a.offset.y, -20.0, 1e-9);
    a = speed_blend_add(t, 1.0, {-300, 0, 80}, level, 1.0);
    CHECK_NEAR(a.offset.y, 0.0, 1e-9);
    a = speed_blend_add(t, 1.0, {-300, 2.5, 80}, level, 1.0); // eased through the centre, so a shoulder swap glides
    CHECK_NEAR(a.offset.y, 10.0, 1e-9);

    // Turned 90 degrees: the view looks along +y, its right is -x.
    const dw::Quat turned = dw::from_rotator(0, 90, 0);
    a = speed_blend_add(t, 1.0, dw::rotate(turned, {-300, 50, 80}), turned, 1.0);
    CHECK_NEAR(a.offset.x, -20.0, 1e-9);
    CHECK_NEAR(a.offset.y, -60.0, 1e-9);
    CHECK_NEAR(a.offset.z, 10.0, 1e-9);

    // A camera in front of the pivot has no arm behind it to lengthen.
    a = speed_blend_add(t, 1.0, {100, 50, 80}, level, 1.0);
    CHECK_NEAR(a.offset.x, 0.0, 1e-9);
}

TEST(speed_blend, eased_speed_and_restart)
{
    SpeedBlend b;
    SpeedBlendTuning t;
    t.amount = 1.0;
    t.start = 150.0;
    t.full = 558.0;
    const SprintView none;
    dw::Vec3 p{0, 0, 0};
    b.restart(p, false);
    // The first reading after a restart is taken as it is.
    p.x += 460.0 * DT;
    b.share(t, none, NAN, p, DT, 1.0);
    CHECK_NEAR(b.speed(), 460.0, 1e-6);
    // Then eased through two stages: a step to 558 starts slower than one stage would move it, and is 98 % there after
    // six rise times (1 - e^-6 * 7 for two equal stages).
    p.x += 558.0 * DT;
    b.share(t, none, NAN, p, DT, 1.0);
    CHECK(b.speed() - 460.0 < (558.0 - 460.0) * (1.0 - std::exp(-DT / t.rise)) * 0.1);
    for (int i = 1; i < 180; ++i)
    {
        p.x += 558.0 * DT;
        b.share(t, none, NAN, p, DT, 1.0);
    }
    CHECK(b.speed() > 460.0 + (558.0 - 460.0) * 0.98 && b.speed() < 558.0);
    // A turn's dip, 0.5 s at 330 from a run of 460, falls on the slower fall: the eased speed stays above 425 (433.8 at
    // the defaults; one 0.4 s stage, before 2026-09-30, went down to 367).
    b.restart(p, false);
    p.x += 460.0 * DT;
    b.share(t, none, NAN, p, DT, 1.0);
    double low = b.speed();
    for (int i = 0; i < 90; ++i)
    {
        p.x += (i < 30 ? 330.0 : 460.0) * DT;
        b.share(t, none, NAN, p, DT, 1.0);
        low = std::min(low, b.speed());
    }
    CHECK(low > 425.0);
    // Vertical motion is not speed.
    const double before = b.speed();
    for (int i = 0; i < 360; ++i)
    {
        p.z += 300.0 * DT;
        b.share(t, none, NAN, p, DT, 1.0);
    }
    CHECK(b.speed() < before * 0.1);
    // A snap under reset_distance is capped.
    b.restart(p, false);
    p.x += 1.0;
    b.share(t, none, NAN, p, DT, 1.0);
    p.x += 450.0;
    b.share(t, none, NAN, p, DT, 1.0);
    CHECK(b.speed() <= SPEED_CAP);
    // A frame with no world time, or a NaN one, reads nothing.
    const double held = b.speed();
    p.x += 100.0;
    b.share(t, none, NAN, p, 0.0, 1.0);
    b.share(t, none, NAN, p, NAN, 1.0);
    CHECK_EQ(b.speed(), held);
}

TEST(speed_blend, share_from_speed_weight_and_context)
{
    SpeedBlendTuning t;
    t.amount = 1.0;
    t.start = 150.0;
    t.full = 558.0;
    SprintView v;
    v.exploring_fov = 90.0f;
    v.sprint_fov = 95.0f;
    auto share_at = [&](double speed, double game_fov, double context, SpeedBlend& b) {
        dw::Vec3 p{0, 0, 0};
        b.restart(p, v.sprint);
        p.x += speed * DT;
        return b.share(t, v, game_fov, p, DT, context);
    };
    SpeedBlend b;
    CHECK_NEAR(share_at(558.0, 90.0, 1.0, b), 1.0, 1e-9); // sprint speed, the game still exploring: all of it
    CHECK_NEAR(share_at(131.0, 90.0, 1.0, b), 0.0, 1e-12); // walk: none
    CHECK_NEAR(share_at(558.0, 92.5, 1.0, b), 0.5, 1e-9);  // the game half into Sprint: the rest
    CHECK_NEAR(share_at(558.0, 95.0, 1.0, b), 0.0, 1e-12); // all Sprint: the game's own
    CHECK_NEAR(share_at(558.0, 90.0, 0.25, b), 0.25, 1e-9); // an override easing in
    t.amount = 0.5;
    CHECK_NEAR(share_at(558.0, 90.0, 1.0, b), 0.5, 1e-9); // speed_blend 50
    t.amount = 0.0;
    CHECK_EQ(share_at(558.0, 90.0, 1.0, b), 0.0);         // off
    CHECK_NEAR(b.speed(), 558.0, 1e-6);                   // but the speed still runs, so switching on shows no step
}

TEST(speed_blend, state_weight_when_the_fov_cannot_tell)
{
    SpeedBlendTuning t;
    t.amount = 1.0;
    t.start = 150.0;
    t.full = 558.0;
    SprintView v;
    v.exploring_fov = 90.0f;
    v.sprint_fov = 95.0f;
    v.fov_moves = true; // an interior FOV override blending in: the FOV says nothing about Sprint
    SpeedBlend b;
    dw::Vec3 p{0, 0, 0};
    b.restart(p, false);
    auto step = [&] {
        p.x += 558.0 * DT;
        return b.share(t, v, 95.0, p, DT, 1.0);
    };
    CHECK_NEAR(step(), 1.0, 1e-9); // not sprinting: all of it, whatever the FOV reads
    v.sprint = true;
    double s = 1.0;
    for (int i = 0; i < 15; ++i) s = step(); // 0.25 s: half the 0.5 s blend in, the S-curve's middle
    CHECK_NEAR(s, 0.5, 1e-9);
    for (int i = 0; i < 15; ++i) s = step();
    CHECK_NEAR(s, 0.0, 1e-9);
    v.sprint = false;
    for (int i = 0; i < 30; ++i) s = step(); // out over 1.0 s: half way after 0.5 s
    CHECK_NEAR(s, 0.5, 1e-9);
    // Within 1 degree: the state too.
    v.fov_moves = false;
    v.sprint_fov = 90.5f;
    v.sprint = true;
    b.restart(p, true);
    CHECK_NEAR(step(), 0.0, 1e-9);
}

TEST(speed_blend, settings_into_the_tuning)
{
    const FollowTuning on = follow_tuning_of(blended_settings());
    CHECK_EQ(on.speed.amount, 1.0);
    CHECK_EQ(on.speed.start, 150.0);
    CHECK_EQ(on.speed.full, 558.0);
    CHECK_NEAR(on.speed.distance_ratio, 1.2, 1e-12);
    CHECK_EQ(on.speed.height, 10.0);
    CHECK_EQ(on.speed.shoulder, 20.0);
    CHECK_EQ(on.speed.fov, 6.0);
    CHECK(!same_values(on, follow_tuning_of(Settings{}))); // a change the core crossfades

    // Off, or with camera_tuning off (the modes hold no Sprint settings then): neutral, so a changed Sprint setting
    // alone bumps no follow generation.
    Settings off = blended_settings();
    off.speed_blend = 0;
    CHECK(same_values(follow_tuning_of(off), follow_tuning_of(Settings{})));
    Settings untuned = blended_settings();
    untuned.camera_tuning = false;
    CHECK(same_values(follow_tuning_of(untuned), follow_tuning_of(Settings{})));
    CHECK(follow_tuning_of(Settings{}).speed == SpeedBlendTuning{});
}

TEST(speed_blend, through_the_follow)
{
    // Sprint speed on the exploring camera: the Sprint group's difference, all of it.
    Run on(blended_settings());
    Run off(Settings{});
    on.walk(558.0, 60);
    off.walk(558.0, 60);
    CHECK_NEAR(on.out.location.x - off.out.location.x, -60.0, 1e-6);
    CHECK_NEAR(on.out.location.y - off.out.location.y, 20.0, 1e-6);
    CHECK_NEAR(on.out.location.z - off.out.location.z, 10.0, 1e-6);
    CHECK_NEAR(on.out.fov_add, 6.0, 1e-6);
    CHECK_EQ(off.out.fov_add, 0.0);

    // At a walk: nothing, to the bit.
    Run walk_on(blended_settings());
    Run walk_off(Settings{});
    walk_on.walk(131.0, 60);
    walk_off.walk(131.0, 60);
    CHECK(same_bits(walk_on.out.location, walk_off.out.location));
    CHECK_EQ(walk_on.out.fov_add, 0.0);

    // Aiming wins: the add eases out with the aiming factor.
    on.inputs.aiming = true;
    on.walk(558.0, 60, false);
    CHECK(std::abs(on.out.fov_add) < 0.01);
    on.inputs.aiming = false;
    // Indoors does not: the interior factor leaves the blend alone.
    on.inputs.interior = true;
    on.walk(558.0, 90, false);
    CHECK_NEAR(on.out.fov_add, 6.0, 1e-3);

    // A restart shows the game's view, add included.
    on.walk(558.0, 1, true);
    CHECK_EQ(on.out.fov_add, 0.0);
    CHECK_EQ(on.out.location.x, on.pivot.x - 300.0);
}

TEST(speed_blend, wall_scales_distance_and_height)
{
    // The game pulls its camera in to half its distance, as for a wall: the clamp's weight is 1 once the recent
    // distance is still long, so the distance and height terms go and the shoulder and the FOV stay.
    Run on(blended_settings());
    Run off(Settings{});
    on.walk(558.0, 60);
    off.walk(558.0, 60);
    on.pivot.x += 558.0 * DT;
    FrameIn in = frame_at(on.pivot);
    in.camera = on.pivot + dw::Vec3{-150, 50, 40};
    on.out = FrameOut{};
    on.follow.frame(on.tuning, on.inputs, in, on.out, on.report);
    off.out = FrameOut{};
    off.follow.frame(off.tuning, off.inputs, in, off.out, off.report);
    CHECK_NEAR(on.out.location.x - off.out.location.x, 0.0, 1e-6);
    CHECK_NEAR(on.out.location.y - off.out.location.y, 20.0, 1e-6);
    CHECK_NEAR(on.out.location.z - off.out.location.z, 0.0, 1e-6);
    CHECK_NEAR(on.out.fov_add, 6.0, 1e-6);
}

TEST(speed_blend, processor_hands_over_the_sprint_view)
{
    FollowProcessor p;
    dw::camera::Processor& core_side = p;
    p.publish(follow_tuning_of(blended_settings()));
    p.set_sprint_view({90.0f, 95.0f, false, false});
    dw::Vec3 pivot{0, 0, 96};
    FrameOut out{};
    auto frame = [&](double game_fov, bool restart = false) {
        if (!restart) pivot.x += 558.0 * DT;
        out = FrameOut{};
        core_side.frame(frame_at(pivot, restart, game_fov), out);
        return out.fov_add;
    };
    frame(90.0, true);
    CHECK_NEAR(frame(90.0), 6.0, 1e-6);  // the game on the exploring FOV
    CHECK_NEAR(frame(92.5), 3.0, 1e-6);  // half way into Sprint
    CHECK_NEAR(frame(95.0), 0.0, 1e-9);  // all Sprint
    p.set_sprint_view({90.0f, 95.0f, true, false});
    CHECK_NEAR(frame(95.0), 6.0, 1e-6);  // a type blend moving the FOV: the state (not sprinting) instead
}
