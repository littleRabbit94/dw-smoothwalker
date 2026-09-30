// Debug markers (debug_markers, debug_key): the camera follow's trail drawn on a plane around the player character,
// as pure numbers so the unit tests compile it. The projector (world to slate units, UE 5.5.4's FMinimalViewInfo
// projection, with near-plane clipping for lines), the followed pivot from the core's view feed, the marker plane's
// lift (the feet are off screen at the default camera, so the plane rises until the dot and the leash fit), the
// trail's ring buffer, the leash circle and the layout of a fixed set of primitives. The widget that draws them is
// ui/marker_layer.hpp; the game-thread gathering is smoothwalker/smoothwalker.cpp, marker_scene. Numbers only: the
// core is reached through camera/api.hpp alone.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "../../camera/api.hpp"
#include "../../common/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string>

namespace dw::smoothwalker::ui::markers
{
    // Counts, times and distances. Sizes are slate units (a 30-unit shape read small at 2880x1800, viewport scale 0.833).
    constexpr int LEASH_DOTS = 32;
    constexpr int TRAIL_DOTS = 25;
    constexpr double TRAIL_SECONDS = 1.0; // s a trail sample lives
    constexpr double TRAIL_STEP = 0.04;   // s at least between two samples
    constexpr double DOT_SIZE = 20.0, RING_SIZE = 40.0, RING_OUTLINE = 4.0, LINE_THICKNESS = 4.0, ARROW_THICKNESS = 4.0;
    constexpr double LEASH_DOT_SIZE = 10.0, LEASH_RIM = 2.0, TRAIL_DOT_NEW = 11.0, TRAIL_DOT_OLD = 6.0, LABEL_FONT = 20.0;
    constexpr double LABEL_GAP = 6.0;             // slate units between the ring's edge and the label's box
    // The label's box, estimated (no layout call): a font point is 4/3 slate units, a Roboto digit about 0.55 em
    // wide. The estimate only sets how far aside the label goes.
    constexpr double LABEL_UNITS_PER_POINT = 4.0 / 3.0, LABEL_GLYPH_EM = 0.55;
    constexpr double ARROW_LENGTH = 60.0;         // cm on the plane
    constexpr double LAG_H_SHOW = 5.0;            // cm of horizontal lag over which the lag line and its label show
    constexpr double LAG_H_HIDE = 4.0;            // and under which they hide again
    constexpr double LAG_V_SHOW = 2.0;            // cm of vertical lag over which the vertical tick shows
    constexpr double LAG_V_HIDE = 1.5;            // and under which it hides again
    constexpr double MIN_DEPTH = 1.0;             // cm in front of the camera a point needs; nearer or behind: not drawn
    constexpr double NEAR_CLIP = 10.0;            // cm in front of the camera a line is clipped to
    constexpr double MIN_VIEWPORT = 64.0;         // px on each axis; less (GetViewportSize's 1x1 without a viewport): nothing drawn
    constexpr double MIN_LEASH = 1.0;             // cm of leash radius; less: the circle would sit on the dot, not drawn
    constexpr double TRAIL_ALPHA_NEW = 0.9, TRAIL_ALPHA_OLD = 0.1;
    constexpr double FIT_BOTTOM = 0.92;           // share of the screen height the dot and the leash must stay above
    constexpr double CAMERA_CLEARANCE = 40.0;     // cm the plane stays below the camera, so it never goes edge-on
    constexpr double LIFT_RATE = 6.0;             // 1/s the applied lift eases toward the target at
    constexpr int LIFT_STEPS = 10;                // bisection steps of the target lift

    struct Rgba
    {
        double r, g, b, a;
    };
    constexpr Rgba AMBER{0.94, 0.62, 0.15, 1.0};
    constexpr Rgba TEAL{0.36, 0.79, 0.65, 1.0};
    constexpr Rgba GREY{0.53, 0.53, 0.50, 1.0};
    constexpr Rgba LINE{0.95, 0.94, 0.91, 0.75}; // lines, the leash and the label
    constexpr Rgba CLEAR{0.0, 0.0, 0.0, 0.0};

    // The primitives, in draw order (later on top). Every index is one Border, built once.
    enum Index : int
    {
        LEASH = 0,                  // LEASH_DOTS dots on the lag limit's circle
        TRAIL = LEASH + LEASH_DOTS, // TRAIL_DOTS dots, TRAIL + 0 the newest
        ARROW_GAME = TRAIL + TRAIL_DOTS,
        ARROW_SHOWN,
        LAG_LINE,
        TICK,
        DOT,
        RING,
        COUNT
    };

    enum class Look
    {
        Box,  // a filled rectangle: lines (rotated) and the tick
        Dot,  // a filled circle
        Ring, // a hollow circle, RING_OUTLINE wide
        Bead, // a filled circle with a dark rim, LEASH_RIM wide: reads on bright ground
    };

    constexpr Rgba RIM{0.0, 0.0, 0.0, 0.7};

    // How the widget at `index` is built: its shape and colour (for a Ring, the outline's; its fill is CLEAR; for a
    // Bead, the fill's, the rim RIM).
    constexpr auto look(int index) -> Look
    {
        if (index == RING) return Look::Ring;
        if (index >= LEASH && index < LEASH + LEASH_DOTS) return Look::Bead;
        if (index == DOT || (index >= TRAIL && index < TRAIL + TRAIL_DOTS)) return Look::Dot;
        return Look::Box;
    }

    constexpr auto color(int index) -> Rgba
    {
        if (index == DOT) return AMBER;
        if (index == RING || index == ARROW_SHOWN || (index >= TRAIL && index < TRAIL + TRAIL_DOTS)) return TEAL;
        if (index == ARROW_GAME) return GREY;
        return LINE; // the leash, the lag line, the tick
    }

    // ------------------------------------------------------------------------------------------ projection

    // The viewport and the projection settings the game reads: pixels, the DPI scale (GetViewportScale), the POV's
    // AspectRatio and bConstrainAspectRatio, and LocalPlayer.AspectRatioAxisConstraint (0 MaintainYFOV,
    // 1 MaintainXFOV, 2 MajorAxisFOV).
    struct Viewport
    {
        double width = 0.0, height = 0.0; // pixels (GetViewportSize)
        double scale = 1.0;               // pixels per slate unit
        double aspect_ratio = 16.0 / 9.0;
        int axis_constraint = 0;
        bool constrain_aspect = false;
    };

    // One view's projection into the viewport. `valid` false: nothing projects.
    struct Projector
    {
        bool valid = false;
        dw::Vec3 origin, forward, right, up;                           // the view's location and axes (UE FRotationMatrix)
        double tan_x = 0.0, tan_y = 0.0;                               // tangents of the half fields of view
        double rect_x = 0.0, rect_y = 0.0, rect_w = 0.0, rect_h = 0.0; // pixels: where the view is drawn
        double scale = 1.0;                                            // pixels per slate unit
    };

    // UE 5.5.4 FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle for `view` (degrees, cm) drawn into
    // `viewport`. Invalid when a view number is not finite, the FOV is outside (0, 180), the viewport is not finite or
    // under MIN_VIEWPORT on either axis, the scale is not positive and finite, or the aspect ratio is not finite (or not
    // positive where it divides).
    inline auto make_projector(const camera::View& view, const Viewport& viewport) -> Projector
    {
        Projector p;
        const double w = viewport.width, h = viewport.height, ar = viewport.aspect_ratio;
        for (double v : {view.location[0], view.location[1], view.location[2], view.rotation[0], view.rotation[1], view.rotation[2]})
            if (!std::isfinite(v)) return p;
        if (!(view.fov > 0.0 && view.fov < 180.0)) return p;
        if (!(std::isfinite(w) && w >= MIN_VIEWPORT && std::isfinite(h) && h >= MIN_VIEWPORT && std::isfinite(viewport.scale) &&
              viewport.scale > 0.0))
            return p;
        if (!std::isfinite(ar)) return p;

        const double half = view.fov * dw::PI / 360.0;
        p.rect_w = w;
        p.rect_h = h;
        if (viewport.constrain_aspect)
        {
            // Black bars: the view keeps its own aspect ratio in a centred rectangle.
            if (!(ar > 0.0)) return p;
            if (w / h > ar) p.rect_w = h * ar;
            else p.rect_h = w / ar;
            p.rect_x = (w - p.rect_w) / 2.0;
            p.rect_y = (h - p.rect_h) / 2.0;
            p.tan_x = std::tan(half);
            p.tan_y = p.tan_x / ar;
        }
        else
        {
            const bool maintain_x = (w > h && viewport.axis_constraint == 2) || viewport.axis_constraint == 1;
            if (maintain_x)
            {
                p.tan_x = std::tan(half);
                p.tan_y = p.tan_x * h / w;
            }
            else
            {
                const double half_y = ar != 0.0 ? std::atan(std::tan(half) / ar) : half;
                p.tan_y = std::tan(half_y);
                p.tan_x = p.tan_y * w / h;
            }
        }
        if (!(std::isfinite(p.tan_x) && p.tan_x > 0.0 && std::isfinite(p.tan_y) && p.tan_y > 0.0)) return p;

        const double r = dw::PI / 180.0;
        const double sp = std::sin(view.rotation[0] * r), cp = std::cos(view.rotation[0] * r);
        const double sy = std::sin(view.rotation[1] * r), cy = std::cos(view.rotation[1] * r);
        const double sr = std::sin(view.rotation[2] * r), cr = std::cos(view.rotation[2] * r);
        p.origin = {view.location[0], view.location[1], view.location[2]};
        p.forward = {cp * cy, cp * sy, sp};
        p.right = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
        p.up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
        p.scale = viewport.scale;
        p.valid = true;
        return p;
    }

    inline auto dot(dw::Vec3 a, dw::Vec3 b) -> double { return a.x * b.x + a.y * b.y + a.z * b.z; }

    // cm along the view axis from the camera; NAN for an invalid projector.
    inline auto depth(const Projector& p, dw::Vec3 world) -> double { return p.valid ? dot(world - p.origin, p.forward) : NAN; }

    // `world` in slate units from the viewport's top left. False (x, y untouched) when the projector is invalid, the
    // point is not finite or it lies less than MIN_DEPTH in front of the camera.
    inline auto project(const Projector& p, dw::Vec3 world, double& x, double& y) -> bool
    {
        if (!p.valid || !std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) return false;
        const dw::Vec3 d = world - p.origin;
        const double depth = dot(d, p.forward);
        if (!(depth >= MIN_DEPTH)) return false;
        const double px = p.rect_x + p.rect_w / 2.0 * (1.0 + dot(d, p.right) / (depth * p.tan_x));
        const double py = p.rect_y + p.rect_h / 2.0 * (1.0 - dot(d, p.up) / (depth * p.tan_y));
        x = px / p.scale;
        y = py / p.scale;
        return true;
    }

    // Clips the segment a-b to depth >= NEAR_CLIP: an end nearer than that moves along the segment to it. False (a, b
    // untouched) when both ends are nearer, either is not finite, or the projector is invalid.
    inline auto clip_near(const Projector& p, dw::Vec3& a, dw::Vec3& b) -> bool
    {
        const double da = depth(p, a), db = depth(p, b);
        if (!std::isfinite(da) || !std::isfinite(db)) return false;
        if (da < NEAR_CLIP && db < NEAR_CLIP) return false;
        if (da < NEAR_CLIP) a = a + (b - a) * ((NEAR_CLIP - da) / (db - da));
        else if (db < NEAR_CLIP) b = b + (a - b) * ((NEAR_CLIP - db) / (da - db));
        return true;
    }

    // ------------------------------------------------------------------------------------------- the pivots

    // The pivot the shown camera follows: pivot - follow_offset (the core's out_offset, before other mods' layers).
    // NAN when either is.
    inline auto shown_pivot(const camera::ViewFeed& v) -> dw::Vec3
    {
        return {v.pivot[0] - v.follow_offset[0], v.pivot[1] - v.follow_offset[1], v.pivot[2] - v.follow_offset[2]};
    }

    // The capsule's bottom: pivot z minus the half height (a NAN half height counts as 0, the capsule centre, which is
    // what the follow tracks then). The marker plane is this plus the lift.
    inline auto feet_z(const camera::ViewFeed& v) -> double
    {
        return v.pivot[2] - (std::isfinite(v.half_height) ? v.half_height : 0.0);
    }

    // The highest lift: the plane stays CAMERA_CLEARANCE below the camera at `camera_z`; never under 0.
    inline auto max_lift(const camera::ViewFeed& v, double camera_z) -> double
    {
        const double lift = camera_z - CAMERA_CLEARANCE - feet_z(v);
        return lift > 0.0 ? lift : 0.0;
    }

    // The markers' world points on the plane at feet_z + lift. Every horizontal marker (the dot, the leash, the
    // trail, the turn arrows) lies on it; the ring keeps its vertical lag above or below it.
    struct Ground
    {
        double plane_z;      // feet_z + lift
        dw::Vec3 game;       // the game's pivot on the plane: the amber dot
        dw::Vec3 ring;       // the followed pivot at plane_z plus its vertical lag: the ring
        dw::Vec3 ring_foot;  // the ring's point on the plane: the tick's foot
        double lag_h, lag_v; // cm, followed pivot minus pivot: horizontal length, vertical (signed, up positive)
    };

    inline auto ground(const camera::ViewFeed& v, double lift) -> Ground
    {
        const dw::Vec3 shown = shown_pivot(v);
        Ground g{};
        g.plane_z = feet_z(v) + lift;
        g.lag_h = std::hypot(v.follow_offset[0], v.follow_offset[1]);
        g.lag_v = -v.follow_offset[2];
        g.game = {v.pivot[0], v.pivot[1], g.plane_z};
        g.ring = {shown.x, shown.y, g.plane_z + g.lag_v};
        g.ring_foot = {shown.x, shown.y, g.plane_z};
        return g;
    }

    // The ring's opacity: 0.35 + 0.65 * keep_follow, keep_follow clamped to 0..1 (NAN: 0), to 0.01.
    inline auto ring_opacity(double keep_follow) -> double
    {
        const double keep = std::isfinite(keep_follow) ? std::clamp(keep_follow, 0.0, 1.0) : 0.0;
        return std::round((0.35 + 0.65 * keep) * 100.0) / 100.0;
    }

    // The leash circle's radius, max_lag_h * keep_follow; NAN (not drawn) when max_lag_h is 0 or either is not
    // finite, or the radius is under MIN_LEASH.
    inline auto leash_radius(double max_lag_h, double keep_follow) -> double
    {
        if (!std::isfinite(max_lag_h) || max_lag_h == 0.0 || !std::isfinite(keep_follow)) return NAN;
        const double radius = max_lag_h * keep_follow;
        return radius >= MIN_LEASH ? radius : NAN;
    }

    // Leash dot `i` of LEASH_DOTS, evenly round `centre` on its horizontal plane, dot 0 along +X, counter-clockwise
    // seen from above.
    inline auto leash_point(dw::Vec3 centre, double radius, int i) -> dw::Vec3
    {
        const double a = 2.0 * dw::PI * i / LEASH_DOTS;
        return {centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), centre.z};
    }

    // Shown with hysteresis: true over `show`, false under `hide`, `shown` in between; false for NAN.
    inline auto hysteresis(bool shown, double value, double show, double hide) -> bool
    {
        if (!std::isfinite(value)) return false;
        if (value > show) return true;
        if (value < hide) return false;
        return shown;
    }

    // ---------------------------------------------------------------------------------------------- the lift

    // True when, on the plane at `lift`, the game's pivot and every leash dot (radius NAN: the dot alone) at least
    // MIN_DEPTH in front of the camera sit at or above FIT_BOTTOM of the screen height (slate y <= FIT_BOTTOM * height
    // / scale). A nearer point is left out of the fit set: lifting cannot bring it in front. Nothing in front fits.
    inline auto fits(const Projector& p, const Viewport& viewport, const camera::ViewFeed& v, double radius, double lift) -> bool
    {
        const double limit = FIT_BOTTOM * viewport.height / viewport.scale;
        const dw::Vec3 centre{v.pivot[0], v.pivot[1], feet_z(v) + lift};
        auto below = [&](dw::Vec3 world) {
            double x = 0.0, y = 0.0;
            return depth(p, world) >= MIN_DEPTH && project(p, world, x, y) && y > limit;
        };
        if (below(centre)) return false;
        if (!std::isfinite(radius)) return true;
        for (int i = 0; i < LEASH_DOTS; ++i)
            if (below(leash_point(centre, radius, i))) return false;
        return true;
    }

    // A lift in [0, max_lift] (the camera being the projector's origin) that fits (see fits), by LIFT_STEPS of
    // bisection that keeps `high` fitting: 0 when the feet already fit; max_lift when even it does not. A plane point
    // in front of and below the camera rises on screen as the plane does, so this is usually the smallest fitting
    // lift to within max_lift / 2^LIFT_STEPS; points that cross MIN_DEPTH as the plane moves (a camera pitched over
    // the character) make fits() non-monotonic, and then it is a fitting lift, not guaranteed the smallest.
    inline auto target_lift(const Projector& p, const Viewport& viewport, const camera::ViewFeed& v, double radius) -> double
    {
        if (fits(p, viewport, v, radius, 0.0)) return 0.0;
        double low = 0.0, high = max_lift(v, p.origin.z);
        if (!fits(p, viewport, v, radius, high)) return high;
        for (int i = 0; i < LIFT_STEPS; ++i)
        {
            const double mid = (low + high) / 2.0;
            if (fits(p, viewport, v, radius, mid)) high = mid;
            else low = mid;
        }
        return high;
    }

    // The lift drawn, eased toward the target so the plane glides instead of jumping.
    class Lift
    {
      public:
        // Once per drawn frame; returns the applied lift. It moves 1 - exp(-LIFT_RATE * dt) of the way to `target`,
        // dt the s since the last update on `now`'s clock. It lands on the target at once on the first update after
        // clear(), when `snap` (the trail was just cleared by a newer snap), or when the clock went back.
        auto update(double target, double now, bool snap) -> double
        {
            if (!std::isfinite(m_applied) || snap || !(now >= m_last)) m_applied = target;
            else m_applied += (target - m_applied) * (1.0 - std::exp(-LIFT_RATE * (now - m_last)));
            m_last = now;
            return m_applied;
        }

        auto clear() -> void { m_applied = m_last = NAN; }
        auto applied() const -> double { return m_applied; } // NAN until the first update after a clear

      private:
        double m_applied = NAN;
        double m_last = NAN; // `now` of the last update
    };

    // --------------------------------------------------------------------------------------------- the trail

    // The followed pivot's recent path, x and y only (drawn on the current plane, so a lift moves it whole): at most
    // TRAIL_DOTS samples, each at least TRAIL_STEP after the one before, dropped at TRAIL_SECONDS old, and all dropped
    // on a newer snap. `now` and `snap_time`: s on the core's clock (ViewFeed::now, DebugFeed::snap_time).
    class Trail
    {
      public:
        struct Sample
        {
            double x, y;
            double t; // `now` when taken
        };

        // Once per drawn frame. A snap later than the last one seen (a NAN snap_time is none; the first finite one
        // after clear() counts as later) clears the trail first, and the call returns true. Then samples TRAIL_SECONDS
        // old or older go, and (x, y) is taken when the trail is empty or its newest sample is TRAIL_STEP old or older.
        // A clock that went back (now before the newest sample) clears the samples too, without returning true.
        auto update(double now, double x, double y, double snap_time) -> bool
        {
            bool snapped = false;
            if (std::isfinite(snap_time) && (!std::isfinite(m_snap_at) || snap_time > m_snap_at))
            {
                m_count = 0;
                m_snap_at = snap_time;
                snapped = true;
            }
            if (m_count > 0 && now < newest().t) m_count = 0;
            while (m_count > 0 && now - oldest().t >= TRAIL_SECONDS) --m_count;
            if (m_count == 0 || now - newest().t >= TRAIL_STEP)
            {
                m_head = (m_head + 1) % TRAIL_DOTS;
                m_samples[m_head] = {x, y, now};
                m_count = std::min(m_count + 1, TRAIL_DOTS);
            }
            return snapped;
        }

        // Hidden, a level change, a new widget: the trail starts again, and the next snap_time seen is the baseline.
        auto clear() -> void
        {
            m_count = 0;
            m_snap_at = NAN;
        }

        auto size() const -> int { return m_count; }
        // 0 the newest, size() - 1 the oldest.
        auto sample(int i) const -> const Sample& { return m_samples[(m_head - i + TRAIL_DOTS) % TRAIL_DOTS]; }

      private:
        auto newest() const -> const Sample& { return sample(0); }
        auto oldest() const -> const Sample& { return sample(m_count - 1); }

        std::array<Sample, TRAIL_DOTS> m_samples{};
        int m_head = 0; // the newest sample's slot
        int m_count = 0;
        double m_snap_at = NAN; // snap_time of the last snap seen
    };

    // What the markers carry from frame to frame: the trail, the applied lift and the hysteresis of the lag line and
    // the tick. One per layer instance.
    struct State
    {
        Trail trail;
        Lift lift;
        bool lag_shown = false;  // the lag line and its label
        bool tick_shown = false; // the vertical tick

        auto clear() -> void
        {
            trail.clear();
            lift.clear();
            lag_shown = tick_shown = false;
        }
    };

    // A trail dot's size and opacity by age: TRAIL_DOT_NEW to TRAIL_DOT_OLD in whole units, TRAIL_ALPHA_NEW to
    // TRAIL_ALPHA_OLD to 0.01, linear over TRAIL_SECONDS (age clamped to 0..TRAIL_SECONDS).
    inline auto trail_size(double age) -> double
    {
        const double a = std::clamp(age / TRAIL_SECONDS, 0.0, 1.0);
        return std::round(TRAIL_DOT_NEW + (TRAIL_DOT_OLD - TRAIL_DOT_NEW) * a);
    }

    inline auto trail_opacity(double age) -> double
    {
        const double a = std::clamp(age / TRAIL_SECONDS, 0.0, 1.0);
        return std::round((TRAIL_ALPHA_NEW + (TRAIL_ALPHA_OLD - TRAIL_ALPHA_NEW) * a) * 100.0) / 100.0;
    }

    // -------------------------------------------------------------------------------------------- the layout

    // One widget's placement: centred on (x, y), w by h, turned angle_deg clockwise on screen, at opacity. All slate
    // units. Hidden: the rest is 0 and not applied.
    struct Primitive
    {
        bool visible = false;
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0, angle_deg = 0.0, opacity = 0.0;
    };

    // The lag line's label, "NN cm", centred on (x, y).
    struct Label
    {
        bool visible = false;
        double x = 0.0, y = 0.0;
        int cm = 0; // horizontal lag, rounded
    };

    inline auto label_text(int cm) -> std::wstring { return std::to_wstring(cm) + L" cm"; }

    // Where the label's centre goes for a lag line from the dot (ax, ay) to the ring (bx, by), on screen: beside the
    // line's midpoint along its normal, to the right (or up, for a level line), far enough that the estimated text box
    // clears the ring, the bigger end. A short line has the dot and the ring on top of each other; above the midpoint
    // the label covered them.
    inline auto label_place(double ax, double ay, double bx, double by, int cm, double& x, double& y) -> void
    {
        const double dx = bx - ax, dy = by - ay, length = std::hypot(dx, dy);
        double nx = 1.0, ny = 0.0; // the ends coincide: to the right
        if (length > 1e-6)
        {
            nx = -dy / length;
            ny = dx / length;
            if (nx < 0.0 || (nx == 0.0 && ny > 0.0))
            {
                nx = -nx;
                ny = -ny;
            }
        }
        const double em = LABEL_FONT * LABEL_UNITS_PER_POINT;
        const double half_w = label_text(cm).size() * LABEL_GLYPH_EM * em / 2.0, half_h = em / 2.0;
        const double away = RING_SIZE / 2.0 + LABEL_GAP + std::abs(nx) * half_w + std::abs(ny) * half_h;
        x = (ax + bx) / 2.0 + nx * away;
        y = (ay + by) / 2.0 + ny * away;
    }

    // What one frame of markers is drawn from, gathered on the game thread.
    struct Scene
    {
        bool readable = false;      // CameraCore::read_view succeeded
        camera::ViewFeed view{};    // what it read: world positions, the follow's lag and yaw, age and the clock
        camera::DebugFeed feed{};   // CameraCore::read_debug
        bool enabled = false;       // Smoothwalker's switch: off, the hook still publishes a pivot while another mod's
                                    // layer is set or the toggle-off fade runs, and nothing is followed
        bool owned = false;         // another mod holds the camera (CameraCore::camera_owner)
        double max_lag_h = 0.0;     // the follow's settings as published
        bool rotation_smoothing = false;
        bool has_camera = false;    // `camera` was read (the widget side fills it)
        camera::View camera{};      // the rendered view, after camera modifiers (PlayerCameraManager's cached POV)
    };

    // The view the markers are projected with: the rendered camera when read, else the feed's shown view.
    inline auto projection_view(const Scene& scene) -> const camera::View& { return scene.has_camera ? scene.camera : scene.view.shown; }

    struct Layout
    {
        std::array<Primitive, COUNT> shapes{};
        Label label;
        double target_lift = NAN;  // cm above the feet where the dot and the leash fit (target_lift); NAN: hidden
        double applied_lift = NAN; // cm above the feet the plane is drawn at (Lift::update); NAN: hidden
    };

    // A dot or ring of `size` at `world`; hidden when it does not project.
    inline auto point(const Projector& p, dw::Vec3 world, double size, double opacity) -> Primitive
    {
        Primitive out;
        if (!project(p, world, out.x, out.y)) return {};
        out.visible = true;
        out.w = out.h = size;
        out.opacity = opacity;
        return out;
    }

    // A line `thickness` thick from a to b, clipped to NEAR_CLIP (clip_near) first: centred between the projected
    // ends, as long as they are apart on screen, turned to their direction, opacity 1. Hidden when both ends are nearer
    // than NEAR_CLIP, an end is not finite, or the ends coincide on screen.
    inline auto line(const Projector& p, dw::Vec3 a, dw::Vec3 b, double thickness) -> Primitive
    {
        double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
        if (!clip_near(p, a, b) || !project(p, a, ax, ay) || !project(p, b, bx, by)) return {};
        const double length = std::hypot(bx - ax, by - ay);
        if (!(length > 0.0)) return {};
        Primitive out;
        out.visible = true;
        out.x = (ax + bx) / 2.0;
        out.y = (ay + by) / 2.0;
        out.w = length;
        out.h = thickness;
        out.angle_deg = std::atan2(by - ay, bx - ax) * 180.0 / dw::PI;
        out.opacity = 1.0;
        return out;
    }

    // True when the scene can be drawn at all: Smoothwalker is on, the feed was read, is 0 to LIVE_WINDOW s old (not
    // paused, in a cutscene or the free camera), has a finite pivot and follow offset, and no other mod holds the
    // camera. Cheap: the widget side asks it before reading the viewport.
    inline auto drawable(const Scene& scene) -> bool
    {
        const auto& v = scene.view;
        if (!scene.enabled || !scene.readable || !(scene.view.age >= 0.0 && scene.view.age <= camera::LIVE_WINDOW) || scene.owned) return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(v.pivot[i]) || !std::isfinite(v.follow_offset[i])) return false;
        return true;
    }

    // One frame: every primitive and the label, projected with projection_view. Not drawable (see drawable) or no
    // valid projection: everything hidden, both lifts NAN, the state cleared. Otherwise, in order: the trail takes the
    // followed pivot's x, y (Trail::update with the feed's snap_time, which reports a newer snap); the target lift is
    // solved for the leash radius (target_lift, leash_radius of max_lag_h and keep_follow) and eased (Lift::update,
    // landing at once after a clear or on that snap); the lag line's and the tick's hysteresis steps; then each
    // primitive shows on the plane at the applied lift (ground) under its own condition:
    //   DOT          Ground::game, DOT_SIZE, opacity 1
    //   RING         Ground::ring, RING_SIZE, ring_opacity(keep_follow)
    //   TICK         Ground::ring down to Ground::ring_foot, LINE_THICKNESS, while |lag_v| is shown (LAG_V_SHOW over,
    //                LAG_V_HIDE under)
    //   LAG_LINE     Ground::game to Ground::ring, LINE_THICKNESS, while lag_h is shown (LAG_H_SHOW over, LAG_H_HIDE
    //                under); the label with it when both ends project, beside the line (label_place), lag_h rounded
    //   LEASH + i    leash_point(Ground::game, radius, i), LEASH_DOT_SIZE, opacity 1, while the radius is not NAN
    //   TRAIL + i    trail sample i (0 newest) at Ground::plane_z, trail_size and trail_opacity of its age
    //   ARROW_GAME   ARROW_LENGTH along the game's yaw from Ground::game, ARROW_THICKNESS; ARROW_SHOWN along
    //                follow_yaw; both while rotation_smoothing is on and keep_turn is finite and above 0
    // A point that does not project, or a line that does not after clipping, is hidden alone.
    inline auto layout(const Scene& scene, const Viewport& viewport, State& state) -> Layout
    {
        Layout out;
        const Projector p = drawable(scene) ? make_projector(projection_view(scene), viewport) : Projector{};
        if (!p.valid)
        {
            state.clear();
            return out;
        }
        const camera::DebugFeed& feed = scene.feed;
        const camera::ViewFeed& v = scene.view;
        const dw::Vec3 shown = shown_pivot(v);
        const bool snapped = state.trail.update(v.now, shown.x, shown.y, feed.snap_time);
        const double radius = leash_radius(scene.max_lag_h, feed.keep_follow);
        out.target_lift = target_lift(p, viewport, v, radius);
        out.applied_lift = state.lift.update(out.target_lift, v.now, snapped);
        const Ground g = ground(v, out.applied_lift);
        state.lag_shown = hysteresis(state.lag_shown, g.lag_h, LAG_H_SHOW, LAG_H_HIDE);
        state.tick_shown = hysteresis(state.tick_shown, std::abs(g.lag_v), LAG_V_SHOW, LAG_V_HIDE);

        out.shapes[DOT] = point(p, g.game, DOT_SIZE, 1.0);
        out.shapes[RING] = point(p, g.ring, RING_SIZE, ring_opacity(feed.keep_follow));
        if (state.tick_shown) out.shapes[TICK] = line(p, g.ring, g.ring_foot, LINE_THICKNESS);
        if (state.lag_shown)
        {
            out.shapes[LAG_LINE] = line(p, g.game, g.ring, LINE_THICKNESS);
            double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
            if (project(p, g.game, ax, ay) && project(p, g.ring, bx, by))
            {
                out.label = {true, 0.0, 0.0, static_cast<int>(std::lround(g.lag_h))};
                label_place(ax, ay, bx, by, out.label.cm, out.label.x, out.label.y);
            }
        }

        if (std::isfinite(radius))
        {
            for (int i = 0; i < LEASH_DOTS; ++i) out.shapes[LEASH + i] = point(p, leash_point(g.game, radius, i), LEASH_DOT_SIZE, 1.0);
        }
        for (int i = 0; i < state.trail.size(); ++i)
        {
            const auto& sample = state.trail.sample(i);
            const double age = v.now - sample.t;
            out.shapes[TRAIL + i] = point(p, {sample.x, sample.y, g.plane_z}, trail_size(age), trail_opacity(age));
        }

        if (scene.rotation_smoothing && std::isfinite(feed.keep_turn) && feed.keep_turn > 0.0)
        {
            auto along = [&](double yaw_deg) {
                const double a = yaw_deg * dw::PI / 180.0;
                return dw::Vec3{g.game.x + ARROW_LENGTH * std::cos(a), g.game.y + ARROW_LENGTH * std::sin(a), g.game.z};
            };
            out.shapes[ARROW_GAME] = line(p, g.game, along(v.game.rotation[1]), ARROW_THICKNESS);
            out.shapes[ARROW_SHOWN] = line(p, g.game, along(v.follow_yaw), ARROW_THICKNESS);
        }
        return out;
    }
} // namespace dw::smoothwalker::ui::markers
