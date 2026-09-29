// The follow: the math behind Smoothwalker's view processor (processor.hpp, camera/frame.hpp). A smoothed pivot
// trails the capsule, the camera becomes smoothed pivot + (game camera - pivot), so orbiting stays instant and only
// following lags. Optional rotation smoothing, the crouch hold, the aiming / combat / traversal shares and the wall
// clamp live here too. docs/design.md, "Follow", "Pivot" and "Walls". Numbers only: runs on the hook thread, no
// Unreal or UE4SS types.
#pragma once

#include "../../camera/frame.hpp"
#include "../../camera/wall.hpp"
#include "../../common/math.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace dw::smoothwalker::follow
{
    // The follow's settings. Numbers only, so the hook's copy allocates nothing on a worker thread.
    struct FollowTuning
    {
        double follow_rate_h = 0.0, follow_rate_v = 0.0;
        int curve_h = 0, curve_v = 0;
        double catchup_distance = 0.0, min_rate_scale = 0.0, max_lag_h = 0.0, max_lag_v = 0.0;
        bool soft_leash = false, rotation_smoothing = false, wall_clamp = false;
        double rotation_rate = 0.0;
        double aiming_keep = 1.0;             // aiming_follow as a share: the trail and turning smoothing kept while aiming
        double combat_follow_keep = 1.0;      // combat_follow as a share: the trail kept while a combat camera is up
        double combat_rotation_keep = 1.0;    // combat_rotation as a share: the turning smoothing kept while a combat camera is up
        double traversal_follow_keep = 1.0;   // traversal_follow as a share: the trail kept while a traversal camera is up
        double traversal_rotation_keep = 1.0; // traversal_rotation as a share: the turning smoothing kept while a traversal camera is up
        double transition = 0.0;              // s, position_transition: the core's crossfade, and a mode write's wall-clamp hold
    };

    inline auto same_values(const FollowTuning& a, const FollowTuning& b) -> bool
    {
        return a.follow_rate_h == b.follow_rate_h && a.follow_rate_v == b.follow_rate_v && a.curve_h == b.curve_h && a.curve_v == b.curve_v &&
               a.catchup_distance == b.catchup_distance && a.min_rate_scale == b.min_rate_scale && a.max_lag_h == b.max_lag_h &&
               a.max_lag_v == b.max_lag_v && a.soft_leash == b.soft_leash && a.rotation_smoothing == b.rotation_smoothing &&
               a.wall_clamp == b.wall_clamp && a.rotation_rate == b.rotation_rate && a.aiming_keep == b.aiming_keep &&
               a.combat_follow_keep == b.combat_follow_keep && a.combat_rotation_keep == b.combat_rotation_keep &&
               a.traversal_follow_keep == b.traversal_follow_keep && a.traversal_rotation_keep == b.traversal_rotation_keep &&
               a.transition == b.transition;
    }

    // What the follow reads besides the core's frame, all Smoothwalker's: a camera-mode write landed since the last
    // frame (its distance glides in over the transition), and which camera-mode groups are blending in or active.
    struct FollowInputs
    {
        bool mode_write = false;
        bool aiming = false, combat = false, traversal = false;
    };

    // The log_stats numbers of one frame.
    struct FollowReport
    {
        bool stats = false;     // a following frame: shown_lag counts
        bool clamped = false;   // the wall clamp moved the result
        double shown_lag = 0.0; // cm between the pivot and the shown pivot
    };

    // Soft: the internal lag may run to 3x the limit and the shown lag eases into it, so the limit has no edge.
    inline auto leash(double lag, double limit, bool soft) -> double
    {
        if (limit <= 0.0) return 0.0;
        return soft ? limit * std::tanh(lag / limit) : std::min(lag, limit);
    }

    // Hook state without a lock: only the player's camera reaches it, and its calls arrive in sequence.
    class Follow
    {
      public:
        auto frame(const FollowTuning& t, const FollowInputs& sw, const camera::FrameIn& in, camera::FrameOut& out, FollowReport& report) -> void
        {
            out.location = in.camera;
            out.rotation = in.rotation;
            out.rotated = false;
            out.feed = false;
            report.stats = false;
            report.clamped = false;

            // A shorter distance gliding in is not a wall. No wall clamp until it has landed.
            if (sw.mode_write) m_nominal_hold = t.transition + 0.3;

            if (in.enabled && in.restart) start(sw, in);
            else if (in.enabled) step(t, sw, in, out, report);

            out.wall_clamp = t.wall_clamp;
            out.nominal_distance = m_nominal_distance;
        }

      private:
        dw::Vec3 m_pivot_smoothed{}; // x, y: the capsule centre; z: the capsule bottom (feet)
        // A crouch or stand: the game eases its camera height over about a third of a second, and the game's own
        // vertical lag (off while the mod is on) used to delay that further. The change is lagged here through the
        // vertical follow: crouch_drop is the game's height change so far, feet-relative (root motion cancels),
        // crouch_smoothed trails it, and the difference holds the camera. An episode starts on a half-height
        // change; crouch_drop stops updating 0.6 s in (the game has settled) so a later pitch change cannot leak.
        double m_half_last = NAN;
        double m_crouch_base = NAN; // camera Z above the feet at the change; NAN: no episode
        double m_crouch_drop = 0.0;
        double m_crouch_smoothed = 0.0;
        double m_crouch_elapsed = 0.0;
        dw::Quat m_rotation_smoothed{};
        double m_nominal_distance = 0.0;
        double m_aim = 0.0;          // 0 to 1, eased toward aiming: how far the follow is handed to the player's aim
        double m_combat = 0.0;       // 0 to 1, eased toward combat: how far the follow is handed to the combat camera
        double m_traversal = 0.0;    // 0 to 1, eased toward traversal: how far the follow is handed to the traversal camera
        double m_nominal_hold = 0.0; // s left in which m_nominal_distance tracks the game: a position write is gliding

        // A crouch drops the capsule centre by the half-height change in one frame (54 cm here) while the game
        // eases its own camera height down over several. Lagging the centre made the arm jump by that delta
        // and the camera pop up. The bottom of the capsule does not move in a crouch, so the vertical follow
        // tracks it: the game's eased height passes through and only real vertical travel is lagged. A missing
        // or implausible half height falls back to the centre.
        static auto feet_z(const camera::FrameIn& in) -> double { return std::isfinite(in.half_height) ? in.pivot.z - in.half_height : in.pivot.z; }

        // From the capsule, showing the game's view.
        auto start(const FollowInputs& sw, const camera::FrameIn& in) -> void
        {
            m_aim = sw.aiming ? 1.0 : 0.0;
            m_combat = sw.combat ? 1.0 : 0.0;
            m_traversal = sw.traversal ? 1.0 : 0.0;
            m_pivot_smoothed = dw::Vec3{in.pivot.x, in.pivot.y, feet_z(in)};
            m_rotation_smoothed = in.rotation;
            m_half_last = in.half_height;
            m_crouch_base = NAN;
            m_crouch_drop = m_crouch_smoothed = 0.0;
            m_nominal_distance = dw::length(in.camera - in.pivot);
        }

        auto step(const FollowTuning& t, const FollowInputs& sw, const camera::FrameIn& in, camera::FrameOut& out, FollowReport& report) -> void
        {
            const dw::Vec3 pivot = in.pivot;
            const dw::Vec3 camera = in.camera;
            const dw::Quat rotation = in.rotation;
            const double half_height = in.half_height;
            const double feet = feet_z(in);
            const double dt = in.dt;

            dw::Vec3& ps = m_pivot_smoothed;
            double inner_h = t.soft_leash ? 3.0 * t.max_lag_h : t.max_lag_h;
            double inner_v = t.soft_leash ? 3.0 * t.max_lag_v : t.max_lag_v;

            double lag_hx = pivot.x - ps.x, lag_hy = pivot.y - ps.y;
            double lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
            double a_h = dw::follow_alpha(t.follow_rate_h, t.curve_h, lag_h, t.catchup_distance, t.min_rate_scale, dt);
            out.rate_h = dw::follow_rate(t.follow_rate_h, t.curve_h, lag_h, t.catchup_distance, t.min_rate_scale);
            ps.x += lag_hx * a_h;
            ps.y += lag_hy * a_h;
            lag_hx = pivot.x - ps.x;
            lag_hy = pivot.y - ps.y;
            lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
            if (lag_h > inner_h && lag_h > 0.0)
            {
                double k = inner_h / lag_h;
                ps.x = pivot.x - lag_hx * k;
                ps.y = pivot.y - lag_hy * k;
                lag_hx *= k;
                lag_hy *= k;
                lag_h = inner_h;
            }

            double lag_v = feet - ps.z;
            double a_v = dw::follow_alpha(t.follow_rate_v, t.curve_v, std::abs(lag_v), t.catchup_distance, t.min_rate_scale, dt);
            ps.z += lag_v * a_v;
            lag_v = feet - ps.z;
            if (std::abs(lag_v) > inner_v)
            {
                lag_v = std::copysign(inner_v, lag_v);
                ps.z = feet - lag_v;
            }

            // A trail behind the crosshair reads as input lag. Only what is shown is scaled, and eased: the smoothed
            // pivot and rotation run on underneath, so the trail returns without an edge.
            double aim_target = sw.aiming ? 1.0 : 0.0;
            m_aim += (aim_target - m_aim) * (1.0 - std::exp(-8.0 * dt));
            double combat_target = sw.combat ? 1.0 : 0.0;
            m_combat += (combat_target - m_combat) * (1.0 - std::exp(-8.0 * dt));
            double traversal_target = sw.traversal ? 1.0 : 0.0;
            m_traversal += (traversal_target - m_traversal) * (1.0 - std::exp(-8.0 * dt));

            // Traversal hands the shown trail and turning to traversal_follow/traversal_rotation as it comes up;
            // combat takes over from wherever traversal left it, and aiming from wherever combat left it, smoothly.
            // Aiming wins over traversal: AimingClawRide and AntiGravAiming stack on top of ClawRide and AntiGrav.
            auto lerp = [](double a, double b, double f) { return a + (b - a) * f; };
            double aiming_keep = std::clamp(t.aiming_keep, 0.0, 1.0);
            double combat_follow_keep = std::clamp(t.combat_follow_keep, 0.0, 1.0);
            double combat_rotation_keep = std::clamp(t.combat_rotation_keep, 0.0, 1.0);
            double traversal_follow_keep = std::clamp(t.traversal_follow_keep, 0.0, 1.0);
            double traversal_rotation_keep = std::clamp(t.traversal_rotation_keep, 0.0, 1.0);
            double keep_pos = lerp(lerp(lerp(1.0, traversal_follow_keep, m_traversal), combat_follow_keep, m_combat), aiming_keep, m_aim);
            double keep_rot = lerp(lerp(lerp(1.0, traversal_rotation_keep, m_traversal), combat_rotation_keep, m_combat), aiming_keep, m_aim);
            out.feed = true;
            out.keep_follow = keep_pos;
            out.keep_turn = keep_rot;
            {
                // The weight each keep carries in the chain above: aiming's, combat's under it, traversal's under
                // both, and the rest (none). They sum to 1; the largest names the influence.
                double w_aim = m_aim;
                double w_combat = m_combat * (1.0 - m_aim);
                double w_traversal = m_traversal * (1.0 - m_combat) * (1.0 - m_aim);
                double weights[4]{1.0 - w_aim - w_combat - w_traversal, w_traversal, w_combat, w_aim}; // in dw::Influence order
                out.influence = static_cast<int>(std::max_element(std::begin(weights), std::end(weights)) - std::begin(weights));
            }

            double shown_h = leash(lag_h, t.max_lag_h, t.soft_leash) * keep_pos;
            double scale_h = lag_h > 0.0 ? shown_h / lag_h : 0.0;
            double shown_v = std::copysign(leash(std::abs(lag_v), t.max_lag_v, t.soft_leash), lag_v) * keep_pos;

            // The crouch hold (docs/design.md, "Crouch hold"). Folding the running hold into a new episode keeps
            // the output continuous when a stand follows a crouch before it has settled.
            double rel = camera.z - feet;
            if (std::isfinite(half_height) && std::isfinite(m_half_last) && half_height != m_half_last)
            {
                double running = m_crouch_drop - m_crouch_smoothed;
                m_crouch_base = rel;
                m_crouch_drop = 0.0;
                m_crouch_smoothed = -running;
                m_crouch_elapsed = 0.0;
            }
            if (std::isfinite(half_height)) m_half_last = half_height;
            double hold = 0.0;
            if (std::isfinite(m_crouch_base))
            {
                m_crouch_elapsed += dt;
                if (m_crouch_elapsed <= 0.6) m_crouch_drop = m_crouch_base - rel;
                double gap = m_crouch_drop - m_crouch_smoothed;
                double a_c = dw::follow_alpha(t.follow_rate_v, t.curve_v, std::abs(gap), t.catchup_distance, t.min_rate_scale, dt);
                m_crouch_smoothed += gap * a_c;
                hold = m_crouch_drop - m_crouch_smoothed;
                if (m_crouch_elapsed > 0.6 && std::abs(hold) < 0.1) m_crouch_base = NAN;
            }
            double shown_hold = std::copysign(leash(std::abs(hold), t.max_lag_v, t.soft_leash), hold) * keep_pos;
            dw::Vec3 shown_pivot{pivot.x - lag_hx * scale_h, pivot.y - lag_hy * scale_h, pivot.z - shown_v + shown_hold};

            // The arm swings with the smoothed rotation so the camera still orbits the pivot.
            dw::Vec3 arm = camera - pivot;
            if (t.rotation_smoothing)
            {
                double a_r = 1.0 - std::exp(-std::max(t.rotation_rate, 0.0) * dt);
                m_rotation_smoothed = dw::slerp(m_rotation_smoothed, rotation, a_r);
                // A trail past 180 degrees would catch up the short way round, which is backwards: at a turning
                // follow speed of 1 a 360 spin reversed the camera halfway (Nexus bug report, 2026-09-21). The
                // trail is capped at 90 degrees, pulled in along the same arc, so the catch-up always runs the
                // way the view turned. A single-frame turn past 180 degrees stays ambiguous, as for any smoothing.
                constexpr double MAX_TRAIL = 0.5 * 3.14159265358979323846;
                double trail = dw::angle_between(m_rotation_smoothed, rotation);
                if (trail > MAX_TRAIL) m_rotation_smoothed = dw::slerp(rotation, m_rotation_smoothed, MAX_TRAIL / trail);
                dw::Quat shown = keep_rot < 1.0 ? dw::slerp(m_rotation_smoothed, rotation, 1.0 - keep_rot) : m_rotation_smoothed;
                dw::Quat delta = dw::multiply(shown, dw::conjugate(rotation));
                arm = dw::rotate(delta, arm);
                out.rotation = shown;
                out.rotated = true;
            }
            else
            {
                m_rotation_smoothed = rotation;
            }

            dw::Vec3 result = shown_pivot + arm;

            // The game has already pulled its camera in front of walls. While it sits closer than usual, the
            // smoothed camera may not be farther out than the game's.
            double game_distance = dw::length(arm);
            double settle = 1.0 - std::exp(-1.0 * dt);
            m_nominal_distance = std::max(game_distance, m_nominal_distance + (game_distance - m_nominal_distance) * settle);
            if (m_nominal_hold > 0.0)
            {
                m_nominal_hold -= dt;
                m_nominal_distance = game_distance;
            }
            if (t.wall_clamp && camera::clamp_to_wall(result, pivot, game_distance, camera::wall_weight(game_distance, m_nominal_distance)))
            {
                report.clamped = true;
            }
            out.location = result;

            report.stats = true;
            report.shown_lag = dw::length(pivot - shown_pivot);
        }
    };
} // namespace dw::smoothwalker::follow
