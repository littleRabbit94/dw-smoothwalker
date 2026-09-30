// The speed blend (docs/design.md, "Speed blend"): the Sprint group's settings arrive with the character's speed
// instead of all at once when the game pushes Sprint. The mode writes already put the Sprint group into the Sprint
// modes, so per frame the follow adds only (sprint - exploration) * share on top of the game's view, with
// share = s * (1 - w) * context: s from the eased horizontal pivot speed, w the Sprint mode's own blend weight, context
// the follow's aiming / combat / focus / traversal factors. Numbers only: runs on the hook thread, no Unreal or UE4SS
// types.
#pragma once

#include "../../common/math.hpp"

#include <algorithm>
#include <cmath>

namespace dw::smoothwalker::follow
{
    // The settings: how much of the Sprint group's difference from the Exploration group's arrives with speed, and that
    // difference. All neutral while the amount is 0, so a changed position setting alone bumps no follow generation.
    struct SpeedBlendTuning
    {
        double amount = 0.0;         // speed_blend as a share; 0 with camera_tuning off: the modes hold no Sprint settings then
        double start = 0.0;          // cm/s at which the blend starts (speed_blend_start)
        double full = 0.0;           // cm/s at which it is complete (speed_blend_full)
        double rise = 0.35;          // s per easing stage while the speed climbs (speed_blend_rise); the defaults run while off
        double fall = 0.75;          // s per easing stage while it drops (speed_blend_fall)
        double distance_ratio = 1.0; // sprint_distance / exploration_distance
        double height = 0.0;         // cm, sprint_height - exploration_height
        double shoulder = 0.0;       // cm, sprint_shoulder - exploration_shoulder
        double fov = 0.0;            // degrees, sprint_fov - exploration_fov
    };

    inline auto operator==(const SpeedBlendTuning& a, const SpeedBlendTuning& b) -> bool
    {
        return a.amount == b.amount && a.start == b.start && a.full == b.full && a.rise == b.rise && a.fall == b.fall &&
               a.distance_ratio == b.distance_ratio && a.height == b.height && a.shoulder == b.shoulder && a.fov == b.fov;
    }

    // What the game thread knows of the Sprint camera (ModeTuner::mode_state): the FOVs as written of the live exploring
    // mode and of Sprint on the live camera type, whether a camera-type blend that moves the FOV is in flight, and
    // whether a Sprint-group mode is blending in or active.
    struct SprintView
    {
        float exploring_fov = NAN, sprint_fov = NAN;
        bool fov_moves = false;
        bool sprint = false;
    };

    inline constexpr double SPEED_CAP = 1500.0;  // cm/s, one frame's reading at most (haste is about 900): a root-motion snap under reset_distance stays a blip
    inline constexpr double SPRINT_IN = 0.5;     // s, Sprint's BlendInArgs: the state weight's rise
    inline constexpr double SPRINT_OUT = 1.0;    // s, Sprint's BlendOutArgs: its fall
    inline constexpr double SIDE_RAMP = 5.0;     // cm of lateral offset over which the shoulder term reaches its full side

    inline auto smoothstep01(double x) -> double
    {
        x = std::clamp(x, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    // s before the amount: 0 at `start` and below, 1 from `full` up. A range with full <= start is a step at start.
    inline auto speed_share(double speed, double start, double full) -> double
    {
        if (!(full > start)) return speed >= start ? 1.0 : 0.0;
        return smoothstep01((speed - start) / (full - start));
    }

    // w from the game's FOV: where it sits between the exploring and the Sprint FOV as written, clamped. NAN when that
    // cannot tell: a FOV unknown, or the two within 1 degree (a sprint_fov that cancels the game's +5).
    inline auto sprint_weight_of_fov(double game_fov, double exploring_fov, double sprint_fov) -> double
    {
        if (!std::isfinite(game_fov) || !std::isfinite(exploring_fov) || !std::isfinite(sprint_fov)) return NAN;
        const double span = sprint_fov - exploring_fov;
        if (!(std::abs(span) >= 1.0)) return NAN;
        return std::clamp((game_fov - exploring_fov) / span, 0.0, 1.0);
    }

    struct SpeedBlendAdd
    {
        dw::Vec3 offset{}; // cm, world space, onto the camera location
        double fov = 0.0;  // degrees onto the game's FOV
    };

    // One frame's add for a share. arm: camera minus pivot as shown (swung with the shown rotation); view: the shown
    // rotation; wall: 1 minus the wall clamp's weight, which scales the distance and height terms. Distance pushes back
    // along the view by the arm's length behind the pivot times (ratio - 1); height goes up in world Z; shoulder goes out
    // along the view's right on the side the camera already sits, eased through the centre and none in a centred mode.
    inline auto speed_blend_add(const SpeedBlendTuning& t, double share, const dw::Vec3& arm, const dw::Quat& view, double wall) -> SpeedBlendAdd
    {
        SpeedBlendAdd add;
        const dw::Vec3 forward = dw::rotate(view, {1.0, 0.0, 0.0});
        const dw::Vec3 right = dw::rotate(view, {0.0, 1.0, 0.0});
        const double behind = std::max(-(arm.x * forward.x + arm.y * forward.y + arm.z * forward.z), 0.0);
        const double lateral = arm.x * right.x + arm.y * right.y + arm.z * right.z;
        const double side = std::clamp(lateral / SIDE_RAMP, -1.0, 1.0);
        add.offset = forward * (-behind * (t.distance_ratio - 1.0) * share * wall) + right * (t.shoulder * share * side);
        add.offset.z += t.height * share * wall;
        add.fov = t.fov * share;
        return add;
    }

    // The eased speed and the Sprint state weight. Hook state without a lock, like the follow that owns it.
    class SpeedBlend
    {
      public:
        // A cut: the easing starts again from the next frame's reading, the state weight from the flag.
        auto restart(const dw::Vec3& pivot, bool sprint) -> void
        {
            m_last = pivot;
            m_primed = false;
            m_stage = m_speed = 0.0;
            m_ramp = sprint ? 1.0 : 0.0;
        }

        // One following frame: reads the pivot's horizontal speed over the world delta and eases it, eases the state
        // weight toward the flag, and answers the share (0 while the amount is 0). game_fov: the game's FOV this frame, or
        // NAN; context: 1 - max(aim, combat, focus, traversal).
        auto share(const SpeedBlendTuning& t, const SprintView& v, double game_fov, const dw::Vec3& pivot, double dt, double context) -> double
        {
            if (dt > 0.0)
            {
                const double dx = pivot.x - m_last.x, dy = pivot.y - m_last.y;
                const double reading = std::min(std::sqrt(dx * dx + dy * dy) / dt, SPEED_CAP);
                if (m_primed)
                {
                    // Two stages in series: the camera starts moving gently rather than with a keyboard's step to a run,
                    // and a slower fall lets a turn's short dip pass while a stop still settles.
                    m_stage = ease(m_stage, reading, t, dt);
                    m_speed = ease(m_speed, m_stage, t, dt);
                }
                else m_stage = m_speed = reading;
                m_primed = true;
                const double target = v.sprint ? 1.0 : 0.0;
                const double step = dt / (target > m_ramp ? SPRINT_IN : SPRINT_OUT);
                m_ramp = target > m_ramp ? std::min(m_ramp + step, target) : std::max(m_ramp - step, target);
            }
            m_last = pivot;
            if (!(t.amount > 0.0)) return 0.0;
            double w = v.fov_moves ? NAN : sprint_weight_of_fov(game_fov, v.exploring_fov, v.sprint_fov);
            if (!std::isfinite(w)) w = smoothstep01(m_ramp);
            const double s = speed_share(m_speed, t.start, t.full) * std::min(t.amount, 1.0);
            return s * (1.0 - w) * std::clamp(context, 0.0, 1.0);
        }

        auto speed() const -> double { return m_speed; }

      private:
        static auto ease(double from, double to, const SpeedBlendTuning& t, double dt) -> double
        {
            const double time = std::max(to > from ? t.rise : t.fall, 1e-3);
            return from + (to - from) * (1.0 - std::exp(-dt / time));
        }

        dw::Vec3 m_last{};
        bool m_primed = false; // a reading since the restart: the first sets the speed, later ones ease it
        double m_stage = 0.0;  // cm/s, the first easing stage
        double m_speed = 0.0;  // cm/s, eased through both
        double m_ramp = 0.0;   // 0 to 1, the Sprint state weight before its S-curve
    };
} // namespace dw::smoothwalker::follow
