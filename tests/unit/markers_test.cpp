// smoothwalker/ui/markers.hpp: the debug markers' projector, near clipping, pivots, lift, trail, hysteresis and
// layout. Expected values are worked out by hand from UE's projection and the header's formulas, not read back from the
// code.

#include "check.hpp"
#include "smoothwalker/ui/markers.hpp"

#include <cmath>

namespace mk = dw::smoothwalker::ui::markers;
namespace cam = dw::camera;

namespace
{
    // The live viewport: MaintainYFOV, AR 16/9, 2880x1800 px at viewport scale 0.833. With fov 90 the half tangents are
    // tan_y = tan(45 deg) / (16/9) = 0.5625 and tan_x = tan_y * 2880/1800 = 0.9.
    constexpr double W = 2880.0, H = 1800.0, SCALE = 0.833;
    constexpr double TAN_X = 0.9, TAN_Y = 0.5625;
    constexpr double TOL = 1e-7;
    constexpr double RAD_TO_DEG = 180.0 / dw::PI;

    auto live_viewport() -> mk::Viewport
    {
        mk::Viewport v;
        v.width = W;
        v.height = H;
        v.scale = SCALE;
        v.aspect_ratio = 16.0 / 9.0;
        v.axis_constraint = 0;
        v.constrain_aspect = false;
        return v;
    }

    auto plain_viewport(double w, double h, double aspect, int axis) -> mk::Viewport
    {
        mk::Viewport v;
        v.width = w;
        v.height = h;
        v.scale = 1.0;
        v.aspect_ratio = aspect;
        v.axis_constraint = axis;
        v.constrain_aspect = false;
        return v;
    }

    auto view_at(double x, double y, double z, double pitch = 0.0, double yaw = 0.0, double roll = 0.0, double fov = 90.0) -> cam::View
    {
        return cam::View{{x, y, z}, {pitch, yaw, roll}, fov};
    }

    // The core's view feed for a game and a shown view that differ by a translation (the follow's lag, no rotation
    // smoothing): follow_offset = game - shown, so the followed pivot is the pivot moved as the view was, and the
    // follow's yaw is the shown one. Age 0.1 s, now 100 s.
    auto snapshot_of(const cam::View& game, const cam::View& shown, dw::Vec3 pivot, double half_height) -> cam::ViewFeed
    {
        cam::ViewFeed s{};
        s.game = game;
        s.shown = shown;
        s.pivot[0] = pivot.x;
        s.pivot[1] = pivot.y;
        s.pivot[2] = pivot.z;
        s.half_height = half_height;
        for (int i = 0; i < 3; ++i) s.follow_offset[i] = game.location[i] - shown.location[i];
        s.follow_yaw = shown.rotation[1];
        s.age = 0.1;
        s.now = 100.0;
        return s;
    }

    // Replaces the scene's view feed, keeping its clock.
    auto set_view(mk::Scene& scene, cam::ViewFeed v) -> void
    {
        v.now = scene.view.now;
        v.age = scene.view.age;
        scene.view = v;
    }

    auto scene_of(const cam::ViewFeed& s) -> mk::Scene
    {
        mk::Scene scene;
        scene.readable = true;
        scene.view = s;
        scene.feed = {};
        scene.feed.keep_follow = 1.0;
        scene.feed.keep_turn = NAN;
        scene.feed.snap_age = NAN;
        scene.feed.snap_time = NAN;
        scene.enabled = true;
        scene.owned = false;
        scene.max_lag_h = 0.0;
        scene.rotation_smoothing = false;
        return scene;
    }

    struct Pt
    {
        double x, y;
    };

    // Slate position of `w` for a camera at `eye` looking along +X, no roll (right +Y, up +Z), on the live viewport:
    // x = W/2 (1 + dy / (depth tan_x)), y = H/2 (1 - dz / (depth tan_y)), both over the viewport scale.
    auto slate_x_forward(dw::Vec3 eye, dw::Vec3 w) -> Pt
    {
        const double depth = w.x - eye.x;
        return {W / 2.0 * (1.0 + (w.y - eye.y) / (depth * TAN_X)) / SCALE, H / 2.0 * (1.0 - (w.z - eye.z) / (depth * TAN_Y)) / SCALE};
    }

    // The same for a camera at `eye` looking along +Y (yaw 90): depth is dy, right is -X, up is +Z.
    auto slate_y_forward(dw::Vec3 eye, dw::Vec3 w) -> Pt
    {
        const double depth = w.y - eye.y;
        return {W / 2.0 * (1.0 + (-(w.x - eye.x)) / (depth * TAN_X)) / SCALE, H / 2.0 * (1.0 - (w.z - eye.z) / (depth * TAN_Y)) / SCALE};
    }

    auto same_color(mk::Rgba a, mk::Rgba b) -> bool { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

    auto expect_prim(const mk::Primitive& p, double x, double y, double w, double h, double angle, double opacity, int line) -> void
    {
        if (!p.visible)
        {
            check::fail(__FILE__, line, "primitive visible");
            return;
        }
        check::near_value(p.x, x, TOL, __FILE__, line, "primitive x");
        check::near_value(p.y, y, TOL, __FILE__, line, "primitive y");
        check::near_value(p.w, w, TOL, __FILE__, line, "primitive w");
        check::near_value(p.h, h, TOL, __FILE__, line, "primitive h");
        // Angles are equal modulo a turn: +180 and -180 are the same line.
        check::near_value(std::remainder(p.angle_deg - angle, 360.0), 0.0, 1e-6, __FILE__, line, "primitive angle");
        check::near_value(p.opacity, opacity, 1e-12, __FILE__, line, "primitive opacity");
    }
#define EXPECT_PRIM(p, x, y, w, h, angle, opacity) expect_prim((p), (x), (y), (w), (h), (angle), (opacity), __LINE__)

    auto everything_hidden(const mk::Layout& out) -> bool
    {
        for (const auto& s : out.shapes)
            if (s.visible) return false;
        return !out.label.visible && std::isnan(out.target_lift) && std::isnan(out.applied_lift);
    }

    // A camera 400 cm behind the pivot and 40 cm below it, so that the marker plane needs no lift; the shown view is the
    // game's moved by `shown_offset`, which is then also the shown pivot's lag. Pivot (0,0,100), half height 96, so the
    // feet (and the plane at lift 0) are at z = 4.
    auto lag_snapshot(dw::Vec3 shown_offset) -> cam::ViewFeed
    {
        return snapshot_of(view_at(-400, 0, 60), view_at(-400 + shown_offset.x, shown_offset.y, 60 + shown_offset.z), {0, 0, 100}, 96.0);
    }

    // The scene the lift was measured on: camera 202 cm behind the pivot and 50 cm above it, level.
    auto live_snapshot() -> cam::ViewFeed { return snapshot_of(view_at(-202, 0, 150), view_at(-202, 0, 150), {0, 0, 100}, 96.0); }
} // namespace

// ------------------------------------------------------------------------------------------------ projection

TEST(markers, project_maintain_y)
{
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), live_viewport());
    CHECK(p.valid);
    CHECK_NEAR(p.tan_x, TAN_X, 1e-12);
    CHECK_NEAR(p.tan_y, TAN_Y, 1e-12);
    CHECK_EQ(p.rect_x, 0.0);
    CHECK_EQ(p.rect_y, 0.0);
    CHECK_EQ(p.rect_w, W);
    CHECK_EQ(p.rect_h, H);
    CHECK_EQ(p.scale, SCALE);
    CHECK_NEAR(p.forward.x, 1.0, 1e-15);
    CHECK_NEAR(p.right.y, 1.0, 1e-15);
    CHECK_NEAR(p.up.z, 1.0, 1e-15);

    double x = 0.0, y = 0.0;
    CHECK(mk::project(p, {500, 0, 0}, x, y)); // straight ahead: the centre, in slate units
    CHECK_NEAR(x, 1440.0 / SCALE, TOL);
    CHECK_NEAR(y, 900.0 / SCALE, TOL);

    CHECK(mk::project(p, {100, 100, 0}, x, y)); // 100 right at depth 100: x = W/2 (1 + 100 / (100 tan_x))
    CHECK_NEAR(x, 1440.0 * (1.0 + 1.0 / 0.9) / SCALE, TOL);
    CHECK_NEAR(y, 900.0 / SCALE, TOL);

    CHECK(mk::project(p, {100, 0, 50}, x, y)); // 50 up at depth 100: y = H/2 (1 - 50 / (100 tan_y)), up is smaller y
    CHECK_NEAR(x, 1440.0 / SCALE, TOL);
    CHECK_NEAR(y, 900.0 * (1.0 - 50.0 / 56.25) / SCALE, TOL);
    CHECK(y < 900.0 / SCALE);

    CHECK(mk::project(p, {100, -100, 0}, x, y)); // left of the screen edge: not clipped, just off screen
    CHECK_NEAR(x, 1440.0 * (1.0 - 1.0 / 0.9) / SCALE, TOL);
    CHECK(x < 0.0);
    CHECK(mk::project(p, {100, 0, -56.25}, x, y)); // the bottom edge exactly
    CHECK_NEAR(y, 1800.0 / SCALE, TOL);
    CHECK(mk::project(p, {200, 180, 0}, x, y)); // depth scales it: the same view angle at twice the distance
    CHECK_NEAR(x, 2880.0 / SCALE, TOL);
}

TEST(markers, project_maintain_x)
{
    // MaintainXFOV: tan_x = tan(45 deg) = 1, tan_y = 1 * 1800/2880 = 0.625.
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), plain_viewport(2880, 1800, 16.0 / 9.0, 1));
    CHECK(p.valid);
    CHECK_NEAR(p.tan_x, 1.0, 1e-12);
    CHECK_NEAR(p.tan_y, 0.625, 1e-12);
    double x = 0.0, y = 0.0;
    CHECK(mk::project(p, {100, 100, 0}, x, y)); // the right edge
    CHECK_NEAR(x, 2880.0, TOL);
    CHECK_NEAR(y, 900.0, TOL);
    CHECK(mk::project(p, {100, 0, 50}, x, y)); // y = 900 (1 - 50/62.5)
    CHECK_NEAR(y, 900.0 * (1.0 - 0.8), TOL);

    // The aspect ratio is not read: a tall viewport keeps tan_x = 1, so tan_y = 2.
    const mk::Projector tall = mk::make_projector(view_at(0, 0, 0), plain_viewport(1000, 2000, 5.0, 1));
    CHECK_NEAR(tall.tan_x, 1.0, 1e-12);
    CHECK_NEAR(tall.tan_y, 2.0, 1e-12);
    CHECK(mk::project(tall, {100, 100, 0}, x, y));
    CHECK_NEAR(x, 1000.0, TOL);
}

TEST(markers, project_major_axis)
{
    double x = 0.0, y = 0.0;
    // Wider than tall: MaintainXFOV. AR 1 tells the two apart: X gives tan_x 1, tan_y 0.5; Y would give 2 and 1.
    mk::Projector p = mk::make_projector(view_at(0, 0, 0), plain_viewport(2000, 1000, 1.0, 2));
    CHECK_NEAR(p.tan_x, 1.0, 1e-12);
    CHECK_NEAR(p.tan_y, 0.5, 1e-12);
    CHECK(mk::project(p, {100, 100, 0}, x, y));
    CHECK_NEAR(x, 1000.0 * (1.0 + 1.0), TOL);

    // Taller than wide: MaintainYFOV: tan_y = 1 / AR = 1, tan_x = 1 * 1000/2000 = 0.5.
    p = mk::make_projector(view_at(0, 0, 0), plain_viewport(1000, 2000, 1.0, 2));
    CHECK_NEAR(p.tan_y, 1.0, 1e-12);
    CHECK_NEAR(p.tan_x, 0.5, 1e-12);
    CHECK(mk::project(p, {100, 100, 0}, x, y));
    CHECK_NEAR(x, 500.0 * (1.0 + 1.0 / 0.5), TOL);

    // Square (H >= W): MaintainYFOV. AR 2: tan_y = 0.5, tan_x = 0.5 (MaintainX would give 1).
    p = mk::make_projector(view_at(0, 0, 0), plain_viewport(1000, 1000, 2.0, 2));
    CHECK_NEAR(p.tan_x, 0.5, 1e-12);
    CHECK_NEAR(p.tan_y, 0.5, 1e-12);
    CHECK(mk::project(p, {100, 50, 0}, x, y));
    CHECK_NEAR(x, 500.0 * (1.0 + 0.5 / 0.5), TOL);
}

TEST(markers, project_aspect_zero)
{
    // AR 0, MaintainYFOV: the half FOV is used as is (fov/2 = 45 deg, tan 1), tan_x = 1 * 1000/500 = 2.
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), plain_viewport(1000, 500, 0.0, 0));
    CHECK(p.valid);
    CHECK_NEAR(p.tan_y, 1.0, 1e-12);
    CHECK_NEAR(p.tan_x, 2.0, 1e-12);
    double x = 0.0, y = 0.0;
    CHECK(mk::project(p, {100, 100, 0}, x, y));
    CHECK_NEAR(x, 500.0 * (1.0 + 100.0 / 200.0), TOL);
    CHECK(mk::project(p, {100, 0, 50}, x, y));
    CHECK_NEAR(y, 250.0 * (1.0 - 0.5), TOL);
}

TEST(markers, project_constrained_aspect)
{
    double x = 0.0, y = 0.0;
    // 2880x1800 is 1.6, narrower than 16/9: black bars above and below. The view is 2880 x 1620 at y 90.
    mk::Viewport v = live_viewport();
    v.scale = 1.0;
    v.constrain_aspect = true;
    mk::Projector p = mk::make_projector(view_at(0, 0, 0), v);
    CHECK(p.valid);
    CHECK_NEAR(p.rect_w, 2880.0, 1e-9);
    CHECK_NEAR(p.rect_h, 1620.0, 1e-9);
    CHECK_NEAR(p.rect_x, 0.0, 1e-9);
    CHECK_NEAR(p.rect_y, 90.0, 1e-9);
    CHECK_NEAR(p.tan_x, 1.0, 1e-12); // the FOV is the horizontal one
    CHECK_NEAR(p.tan_y, 0.5625, 1e-12);
    CHECK(mk::project(p, {500, 0, 0}, x, y)); // still the centre of the screen
    CHECK_NEAR(x, 1440.0, TOL);
    CHECK_NEAR(y, 900.0, TOL);
    CHECK(mk::project(p, {100, 100, 56.25}, x, y)); // the top right corner of the view rectangle
    CHECK_NEAR(x, 2880.0, TOL);
    CHECK_NEAR(y, 90.0, TOL);
    CHECK(mk::project(p, {100, -100, -56.25}, x, y)); // the bottom left corner
    CHECK_NEAR(x, 0.0, TOL);
    CHECK_NEAR(y, 1710.0, TOL);

    // 4000x1800 is wider than 16/9: black bars left and right. The view is 3200 x 1800 at x 400. The axis constraint
    // does not matter with a constrained aspect ratio.
    v.width = 4000.0;
    v.axis_constraint = 2;
    p = mk::make_projector(view_at(0, 0, 0), v);
    CHECK_NEAR(p.rect_w, 3200.0, 1e-9);
    CHECK_NEAR(p.rect_h, 1800.0, 1e-9);
    CHECK_NEAR(p.rect_x, 400.0, 1e-9);
    CHECK_NEAR(p.rect_y, 0.0, 1e-9);
    CHECK(mk::project(p, {100, 100, 0}, x, y));
    CHECK_NEAR(x, 3600.0, TOL);
    CHECK(mk::project(p, {100, -100, 0}, x, y));
    CHECK_NEAR(x, 400.0, TOL);

    // The rectangle is in pixels: the slate result is over the scale.
    v.scale = 2.0;
    p = mk::make_projector(view_at(0, 0, 0), v);
    CHECK(mk::project(p, {100, 100, 0}, x, y));
    CHECK_NEAR(x, 1800.0, TOL);
    CHECK_NEAR(y, 450.0, TOL);
}

TEST(markers, project_rotated_views)
{
    double x = 0.0, y = 0.0;
    // Yaw 90, pitch -30: forward (0, cos 30, -0.5), right (-1, 0, 0), up (0, 0.5, cos 30).
    const double c30 = std::sqrt(3.0) / 2.0;
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0, -30, 90, 0), live_viewport());
    CHECK(p.valid);
    CHECK_NEAR(p.forward.x, 0.0, 1e-12);
    CHECK_NEAR(p.forward.y, c30, 1e-12);
    CHECK_NEAR(p.forward.z, -0.5, 1e-12);
    CHECK_NEAR(p.right.x, -1.0, 1e-12);
    CHECK_NEAR(p.up.y, 0.5, 1e-12);
    CHECK_NEAR(p.up.z, c30, 1e-12);

    CHECK(mk::project(p, {0, 500 * c30, -250}, x, y)); // 500 along the view axis: the centre
    CHECK_NEAR(x, 1440.0 / SCALE, TOL);
    CHECK_NEAR(y, 900.0 / SCALE, TOL);
    // 300 along the axis plus 100 to the view's right, which is world -X: right of centre, level.
    CHECK(mk::project(p, {-100, 300 * c30, -150}, x, y));
    CHECK_NEAR(x, 1440.0 * (1.0 + 100.0 / (300.0 * 0.9)) / SCALE, TOL);
    CHECK_NEAR(y, 900.0 / SCALE, TOL);
    CHECK(x > 1440.0 / SCALE);
    // 300 along the axis plus 60 along the view's up.
    CHECK(mk::project(p, {0, 300 * c30 + 60 * 0.5, -150 + 60 * c30}, x, y));
    CHECK_NEAR(x, 1440.0 / SCALE, TOL);
    CHECK_NEAR(y, 900.0 * (1.0 - 60.0 / (300.0 * 0.5625)) / SCALE, TOL);

    // Roll 90 (FRotationMatrix): right is world -Z, up is world +Y. So a point on world +Y, right of centre at roll 0,
    // is above the centre; world -Z is right of centre; world +Z is left of it.
    const mk::Projector r = mk::make_projector(view_at(0, 0, 0, 0, 0, 90), live_viewport());
    CHECK_NEAR(r.right.z, -1.0, 1e-12);
    CHECK_NEAR(r.up.y, 1.0, 1e-12);
    CHECK(mk::project(r, {100, 50, 0}, x, y));
    CHECK_NEAR(x, 1440.0 / SCALE, TOL);
    CHECK_NEAR(y, 900.0 * (1.0 - 50.0 / 56.25) / SCALE, TOL);
    CHECK(y < 900.0 / SCALE);
    CHECK(mk::project(r, {100, 0, -50}, x, y));
    CHECK_NEAR(x, 1440.0 * (1.0 + 50.0 / 90.0) / SCALE, TOL);
    CHECK_NEAR(y, 900.0 / SCALE, TOL);
    CHECK(mk::project(r, {100, 0, 50}, x, y));
    CHECK_NEAR(x, 1440.0 * (1.0 - 50.0 / 90.0) / SCALE, TOL);
}

TEST(markers, project_from_a_translated_view)
{
    // The view's location is the origin of the depth and side distances.
    const mk::Projector p = mk::make_projector(view_at(1000, 2000, 300), live_viewport());
    double x = 0.0, y = 0.0;
    CHECK(mk::project(p, {1200, 2090, 300 - 56.25 * 2}, x, y)); // depth 200, 90 right, 112.5 down
    CHECK_NEAR(x, 1440.0 * (1.0 + 90.0 / (200.0 * 0.9)) / SCALE, TOL);
    CHECK_NEAR(y, 1800.0 / SCALE, TOL);
}

TEST(markers, project_live_anchor)
{
    // The measured setup (MaintainY, AR 1.7778, 2880x1800, scale 0.8330), one point of my own choosing: a camera at
    // (1000, 2000, 300) facing +X, a point 202 ahead, 85 to the right and 150 below the eye. tan_y = 1 / 1.7778 (fov 90),
    // tan_x = tan_y * 1.6.
    mk::Viewport v = live_viewport();
    v.aspect_ratio = 1.7778;
    v.scale = 0.8330;
    const mk::Projector p = mk::make_projector(view_at(1000, 2000, 300), v);
    CHECK(p.valid);
    const double tan_y = 1.0 / 1.7778, tan_x = tan_y * 1.6;
    double x = 0.0, y = 0.0;
    CHECK(mk::project(p, {1202, 2085, 150}, x, y));
    CHECK_NEAR(x, 1440.0 * (1.0 + 85.0 / (202.0 * tan_x)) / 0.8330, 1e-6);
    CHECK_NEAR(y, 900.0 * (1.0 + 150.0 / (202.0 * tan_y)) / 0.8330, 1e-6);
}

TEST(markers, project_refusals)
{
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), live_viewport());
    double x = 7.0, y = 8.0;
    CHECK(!mk::project(p, {-100, 0, 0}, x, y)); // behind the camera
    CHECK(!mk::project(p, {0, 0, 0}, x, y));    // at the eye
    CHECK(!mk::project(p, {0.999, 0, 0}, x, y)); // in front but under MIN_DEPTH
    CHECK_EQ(x, 7.0);
    CHECK_EQ(y, 8.0);
    CHECK(mk::project(p, {1.0, 0, 0}, x, y)); // exactly MIN_DEPTH draws
    CHECK_NEAR(x, 1440.0 / SCALE, TOL);
    x = 7.0;
    y = 8.0;
    CHECK(!mk::project(p, {NAN, 0, 0}, x, y));
    CHECK(!mk::project(p, {100, INFINITY, 0}, x, y));
    CHECK(!mk::project(p, {100, 0, -INFINITY}, x, y));
    CHECK_EQ(x, 7.0);
    CHECK_EQ(y, 8.0);

    // Depth is measured along the forward axis, not the distance: a point beside the eye is not in front.
    CHECK(!mk::project(p, {0.5, 100, 0}, x, y));
    CHECK_EQ(x, 7.0);
}

TEST(markers, invalid_projectors)
{
    const cam::View good = view_at(0, 0, 0);
    const mk::Viewport vp = live_viewport();
    CHECK(mk::make_projector(good, vp).valid);
    CHECK(mk::make_projector(view_at(0, 0, 0, 0, 0, 0, 1.0), vp).valid);
    CHECK(mk::make_projector(view_at(0, 0, 0, 0, 0, 0, 179.0), vp).valid);
    CHECK(!mk::Projector{}.valid);

    for (int i = 0; i < 6; ++i)
    {
        cam::View v = good;
        (i < 3 ? v.location[i] : v.rotation[i - 3]) = NAN;
        CHECK(!mk::make_projector(v, vp).valid);
    }
    for (double fov : {0.0, 180.0, 190.0, -10.0, static_cast<double>(NAN), static_cast<double>(INFINITY)})
        CHECK(!mk::make_projector(view_at(0, 0, 0, 0, 0, 0, fov), vp).valid);

    mk::Viewport v = vp;
    v.width = 0.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.height = 0.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.width = -5.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.width = NAN;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.height = INFINITY;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.scale = 0.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.scale = -1.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.scale = NAN;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.aspect_ratio = NAN;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.aspect_ratio = INFINITY;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.aspect_ratio = -1.0; // MaintainY would flip tan_y negative
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.constrain_aspect = true;
    v.aspect_ratio = 0.0; // the constrained rectangle divides by it
    CHECK(!mk::make_projector(good, v).valid);

    // GetViewportSize without a game viewport answers 1x1: under MIN_VIEWPORT (64 px) on either axis, nothing projects.
    v = vp;
    v.width = v.height = 1.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.width = 63.9;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.height = 63.0;
    CHECK(!mk::make_projector(good, v).valid);
    v = vp;
    v.width = v.height = 64.0;
    CHECK(mk::make_projector(good, v).valid);

    // An invalid projector projects nothing, and leaves x and y alone.
    const mk::Projector bad = mk::make_projector(view_at(0, 0, 0, 0, 0, 0, 0.0), vp);
    double x = 7.0, y = 8.0;
    CHECK(!mk::project(bad, {500, 0, 0}, x, y));
    CHECK_EQ(x, 7.0);
    CHECK_EQ(y, 8.0);
}

// ---------------------------------------------------------------------------------------------- pivots and lift

TEST(markers, shown_pivot)
{
    // The followed pivot is pivot - follow_offset, whatever the views say: the shown view carries other mods' layers.
    cam::ViewFeed s = snapshot_of(view_at(-400, 0, 60), view_at(-400, 0, 60), {10, 20, 5}, 96);
    dw::Vec3 sp = mk::shown_pivot(s);
    CHECK_EQ(sp.x, 10.0);
    CHECK_EQ(sp.y, 20.0);
    CHECK_EQ(sp.z, 5.0);

    s.follow_offset[0] = 30.0;
    s.follow_offset[1] = -40.0;
    s.follow_offset[2] = 7.0;
    s.shown.location[0] += 500.0; // a layer moved the shown view: not part of the follow's lag
    s.shown.rotation[1] = 45.0;
    sp = mk::shown_pivot(s);
    CHECK_EQ(sp.x, -20.0);
    CHECK_EQ(sp.y, 60.0);
    CHECK_EQ(sp.z, -2.0);

    s.follow_offset[1] = NAN;
    CHECK(std::isnan(mk::shown_pivot(s).y));
    s.follow_offset[1] = 0.0;
    s.pivot[2] = NAN;
    CHECK(std::isnan(mk::shown_pivot(s).z));
}

TEST(markers, feet_and_max_lift)
{
    cam::ViewFeed s = snapshot_of(view_at(0, 0, 0), view_at(0, 0, 105), {0, 0, 100}, 96);
    CHECK_EQ(mk::feet_z(s), 4.0);
    CHECK_EQ(mk::max_lift(s, 105.0), 105.0 - 40.0 - 4.0);
    s.half_height = NAN; // the capsule centre stands in for the feet
    CHECK_EQ(mk::feet_z(s), 100.0);
    CHECK_EQ(mk::max_lift(s, 105.0), 0.0);
    s.half_height = 96.0;
    CHECK_EQ(mk::max_lift(s, 30.0), 0.0); // the camera below the plane's ceiling: 30 - 40 - 4 = -14, never negative
    CHECK_EQ(mk::max_lift(s, 44.0), 0.0); // exactly at the clearance
    CHECK_EQ(mk::max_lift(s, 144.0), 100.0);
    s.half_height = INFINITY; // not finite: as unknown
    CHECK_EQ(mk::feet_z(s), 100.0);
}

TEST(markers, ground_fields)
{
    // follow_offset (-30, -40, 7): the followed pivot is (40, 60, 93), 50 cm off horizontally and 7 below. The pivot
    // is (10, 20, 100), the feet at z = 4, lift 10, so the plane is at 14.
    cam::ViewFeed s = snapshot_of(view_at(-400, 0, 60), view_at(-370, 40, 53), {10, 20, 100}, 96);
    mk::Ground g = mk::ground(s, 10.0);
    CHECK_NEAR(g.plane_z, 14.0, 1e-9);
    CHECK_NEAR(g.lag_h, 50.0, 1e-9);
    CHECK_NEAR(g.lag_v, -7.0, 1e-9);
    CHECK_NEAR(g.game.x, 10.0, 1e-9);
    CHECK_NEAR(g.game.y, 20.0, 1e-9);
    CHECK_NEAR(g.game.z, 14.0, 1e-9);
    CHECK_NEAR(g.ring.x, 40.0, 1e-9);
    CHECK_NEAR(g.ring.y, 60.0, 1e-9);
    CHECK_NEAR(g.ring.z, 7.0, 1e-9); // the plane plus the lag: 14 - 7
    CHECK_NEAR(g.ring_foot.x, 40.0, 1e-9);
    CHECK_NEAR(g.ring_foot.y, 60.0, 1e-9);
    CHECK_NEAR(g.ring_foot.z, 14.0, 1e-9);

    // Up positive: the followed pivot above the pivot.
    s.follow_offset[2] = -13.0;
    g = mk::ground(s, 0.0);
    CHECK_NEAR(g.lag_v, 13.0, 1e-9);
    CHECK_NEAR(g.plane_z, 4.0, 1e-9);
    CHECK_NEAR(g.ring.z, 17.0, 1e-9);
    CHECK_NEAR(g.ring_foot.z, 4.0, 1e-9);

    // No half height: the plane is measured from the pivot.
    s.half_height = NAN;
    g = mk::ground(s, 5.0);
    CHECK_NEAR(g.plane_z, 105.0, 1e-9);
}

TEST(markers, ring_opacity)
{
    CHECK_NEAR(mk::ring_opacity(0.0), 0.35, 1e-12);
    CHECK_NEAR(mk::ring_opacity(1.0), 1.0, 1e-12);
    CHECK_NEAR(mk::ring_opacity(0.2), 0.48, 1e-12); // 0.35 + 0.13
    CHECK_NEAR(mk::ring_opacity(0.4), 0.61, 1e-12); // 0.35 + 0.26
    CHECK_NEAR(mk::ring_opacity(2.5), 1.0, 1e-12);  // clamped
    CHECK_NEAR(mk::ring_opacity(-3.0), 0.35, 1e-12);
    CHECK_NEAR(mk::ring_opacity(NAN), 0.35, 1e-12);
    CHECK_NEAR(mk::ring_opacity(0.123456), 0.43, 1e-12); // 0.35 + 0.08025 = 0.43025, to 0.01
}

TEST(markers, leash_radius_and_points)
{
    CHECK_NEAR(mk::leash_radius(150.0, 0.5), 75.0, 1e-12);
    CHECK_NEAR(mk::leash_radius(85.0, 1.0), 85.0, 1e-12);
    CHECK_NEAR(mk::leash_radius(2.0, 0.5), 1.0, 1e-12); // exactly MIN_LEASH draws
    CHECK(std::isnan(mk::leash_radius(0.0, 1.0)));      // no lag limit
    CHECK(std::isnan(mk::leash_radius(NAN, 1.0)));
    CHECK(std::isnan(mk::leash_radius(150.0, NAN)));
    CHECK(std::isnan(mk::leash_radius(INFINITY, 1.0)));
    CHECK(std::isnan(mk::leash_radius(150.0, INFINITY)));
    CHECK(std::isnan(mk::leash_radius(150.0, 0.006))); // 0.9 cm: under MIN_LEASH
    CHECK(std::isnan(mk::leash_radius(150.0, 0.0)));
    CHECK(std::isnan(mk::leash_radius(150.0, -1.0)));

    const dw::Vec3 c{10, 20, 30};
    dw::Vec3 q = mk::leash_point(c, 85.0, 0); // dot 0 along +X
    CHECK_NEAR(q.x, 95.0, 1e-9);
    CHECK_NEAR(q.y, 20.0, 1e-9);
    CHECK_NEAR(q.z, 30.0, 1e-12);
    q = mk::leash_point(c, 85.0, 8); // a quarter turn counter-clockwise from above: +Y
    CHECK_NEAR(q.x, 10.0, 1e-9);
    CHECK_NEAR(q.y, 105.0, 1e-9);
    CHECK_NEAR(q.z, 30.0, 1e-12);
    q = mk::leash_point(c, 85.0, 16);
    CHECK_NEAR(q.x, -75.0, 1e-9);
    CHECK_NEAR(q.y, 20.0, 1e-9);
    q = mk::leash_point(c, 85.0, 24);
    CHECK_NEAR(q.x, 10.0, 1e-9);
    CHECK_NEAR(q.y, -65.0, 1e-9);
    q = mk::leash_point({0, 0, 0}, 2.0, 4); // an eighth of a turn
    CHECK_NEAR(q.x, std::sqrt(2.0), 1e-12);
    CHECK_NEAR(q.y, std::sqrt(2.0), 1e-12);
}

TEST(markers, fits_bounds_on_the_live_scene)
{
    // Camera 202 behind the pivot, 50 above it, level, fov 90; feet at z = 4, so the plane at lift L is 146 - L below
    // the eye. A point at depth d lands at py = 900 + 900 (146 - L) / (d 0.5625) pixels and must be at or above
    // 0.92 * 1800 = 1656: 146 - L <= 0.84 d 0.5625.
    const cam::ViewFeed s = live_snapshot();
    const mk::Viewport vp = live_viewport();
    const mk::Projector p = mk::make_projector(s.shown, vp);
    CHECK_EQ(mk::max_lift(s, p.origin.z), 106.0);

    // The dot alone (radius NAN), depth 202: fits from L = 146 - 0.84 * 202 * 0.5625 = 50.555.
    const double dot_edge = 146.0 - 0.84 * 202.0 * 0.5625;
    CHECK(!mk::fits(p, vp, s, NAN, 0.0));
    CHECK(!mk::fits(p, vp, s, NAN, dot_edge - 0.01));
    CHECK(mk::fits(p, vp, s, NAN, dot_edge + 0.01));
    CHECK(mk::fits(p, vp, s, NAN, 106.0));

    // The leash at radius 85 adds the dot at (-85, 0), depth 117: L = 146 - 0.84 * 117 * 0.5625 = 90.7175.
    const double leash_edge = 146.0 - 0.84 * 117.0 * 0.5625;
    CHECK(!mk::fits(p, vp, s, 85.0, dot_edge + 0.01)); // the dot fits, the leash does not
    CHECK(!mk::fits(p, vp, s, 85.0, 60.0));            // measured in game: +60 clipped the leash
    CHECK(!mk::fits(p, vp, s, 85.0, leash_edge - 0.01));
    CHECK(mk::fits(p, vp, s, 85.0, leash_edge + 0.01));
    CHECK(mk::fits(p, vp, s, 85.0, 106.0));
}

TEST(markers, target_lift_on_the_live_scene)
{
    const cam::ViewFeed s = live_snapshot();
    const mk::Viewport vp = live_viewport();
    const mk::Projector p = mk::make_projector(s.shown, vp);
    const double step = mk::max_lift(s, p.origin.z) / 1024.0; // 10 bisection steps of [0, 106]

    // Leash 85: the geometry puts the threshold at 90.7175 cm (see fits_bounds_on_the_live_scene); the bisection
    // lands on the first multiple of 106/1024 above it. In the measured game run +60 clipped the leash.
    const double leash_edge = 146.0 - 0.84 * 117.0 * 0.5625;
    const double lift = mk::target_lift(p, vp, s, 85.0);
    CHECK(lift > 60.0);
    CHECK(lift >= leash_edge);
    CHECK(lift <= leash_edge + step);
    CHECK(mk::fits(p, vp, s, 85.0, lift));
    CHECK(!mk::fits(p, vp, s, 85.0, lift - step * 1.001)); // the bisection's bound: one step lower does not fit

    // The dot alone.
    const double dot_edge = 146.0 - 0.84 * 202.0 * 0.5625;
    const double dot_lift = mk::target_lift(p, vp, s, NAN);
    CHECK(dot_lift >= dot_edge);
    CHECK(dot_lift <= dot_edge + step);
    CHECK(mk::fits(p, vp, s, NAN, dot_lift));
    CHECK(!mk::fits(p, vp, s, NAN, dot_lift - step * 1.001));
    CHECK(dot_lift < lift);
}

TEST(markers, target_lift_extremes)
{
    const mk::Viewport vp = live_viewport();

    // It fits with no lift at all: 0.
    cam::ViewFeed s = lag_snapshot({0, 0, 0});
    mk::Projector p = mk::make_projector(s.shown, vp);
    CHECK(mk::fits(p, vp, s, 85.0, 0.0));
    CHECK_EQ(mk::target_lift(p, vp, s, 85.0), 0.0);
    CHECK_EQ(mk::target_lift(p, vp, s, NAN), 0.0);

    // Nothing fits even at max_lift: the camera 20 cm from the pivot, 50 above. At the top (106) the dot is at
    // py = 900 + 900 * 40 / (20 * 0.5625) = 4100.
    s = snapshot_of(view_at(-20, 0, 150), view_at(-20, 0, 150), {0, 0, 100}, 96);
    p = mk::make_projector(s.shown, vp);
    CHECK_EQ(mk::max_lift(s, p.origin.z), 106.0);
    CHECK(!mk::fits(p, vp, s, NAN, 0.0));
    CHECK(!mk::fits(p, vp, s, NAN, 106.0));
    CHECK_EQ(mk::target_lift(p, vp, s, NAN), 106.0);
    CHECK_EQ(mk::target_lift(p, vp, s, 85.0), 106.0);

    // Nothing projects (the pivot and the leash are behind the camera): nothing to clip, it fits.
    s = snapshot_of(view_at(200, 0, 150), view_at(200, 0, 150), {0, 0, 100}, 96);
    p = mk::make_projector(s.shown, vp);
    CHECK(mk::fits(p, vp, s, NAN, 0.0));
    CHECK(mk::fits(p, vp, s, 85.0, 0.0));
    CHECK_EQ(mk::target_lift(p, vp, s, 85.0), 0.0);

    // An invalid projector projects nothing either.
    const mk::Projector bad;
    CHECK(mk::fits(bad, vp, s, 85.0, 0.0));
}

TEST(markers, lift_easing)
{
    mk::Lift lift;
    CHECK(std::isnan(lift.applied()));
    CHECK_EQ(lift.update(10.0, 5.0, false), 10.0); // the first update lands on the target
    CHECK_EQ(lift.applied(), 10.0);

    // Later updates close 1 - exp(-6 dt) of the way.
    const double a = 10.0 + (20.0 - 10.0) * (1.0 - std::exp(-0.6));
    CHECK_NEAR(lift.update(20.0, 5.1, false), a, 1e-12);
    CHECK_NEAR(lift.applied(), a, 1e-12);
    const double b = a + (0.0 - a) * (1.0 - std::exp(-6.0 * 0.25));
    CHECK_NEAR(lift.update(0.0, 5.35, false), b, 1e-12);
    CHECK_NEAR(lift.update(0.0, 5.35, false), b, 1e-12); // no time passed: no move

    CHECK_EQ(lift.update(42.0, 5.4, true), 42.0); // snap: at once
    CHECK_EQ(lift.applied(), 42.0);
    CHECK_NEAR(lift.update(0.0, 5.5, false), 42.0 * std::exp(-0.6), 1e-12);

    // The clock went back: at once, and the new time is the reference for the next update.
    CHECK_EQ(lift.update(7.0, 1.0, false), 7.0);
    CHECK_NEAR(lift.update(17.0, 1.1, false), 7.0 + 10.0 * (1.0 - std::exp(-0.6)), 1e-12);

    lift.clear();
    CHECK(std::isnan(lift.applied()));
    CHECK_EQ(lift.update(3.0, 9.0, false), 3.0); // lands again after a clear
}

// ---------------------------------------------------------------------------------------------------- the trail

TEST(markers, trail_cadence)
{
    mk::Trail t;
    CHECK_EQ(t.size(), 0);
    CHECK(!t.update(0.0, 1.0, 2.0, NAN)); // the first update takes a sample
    CHECK_EQ(t.size(), 1);
    CHECK_EQ(t.sample(0).x, 1.0);
    CHECK_EQ(t.sample(0).y, 2.0);
    CHECK_EQ(t.sample(0).t, 0.0);
    CHECK(!t.update(0.03125, 3.0, 4.0, NAN)); // under 0.04 s: no sample
    CHECK_EQ(t.size(), 1);
    CHECK_EQ(t.sample(0).x, 1.0);
    CHECK(!t.update(0.0625, 5.0, 6.0, NAN)); // 0.0625 s after the first: a sample, newest at index 0
    CHECK_EQ(t.size(), 2);
    CHECK_EQ(t.sample(0).x, 5.0);
    CHECK_EQ(t.sample(0).y, 6.0);
    CHECK_EQ(t.sample(0).t, 0.0625);
    CHECK_EQ(t.sample(1).x, 1.0);
    CHECK(!t.update(0.125, 7.0, 8.0, NAN));
    CHECK_EQ(t.size(), 3);
    CHECK_EQ(t.sample(0).x, 7.0);
    CHECK_EQ(t.sample(1).x, 5.0);
    CHECK_EQ(t.sample(2).x, 1.0);

    // Exactly 0.04 s apart counts.
    mk::Trail u;
    u.update(0.0, 1.0, 1.0, NAN);
    u.update(0.04, 2.0, 2.0, NAN);
    CHECK_EQ(u.size(), 2);
    CHECK_EQ(u.sample(0).x, 2.0);
}

TEST(markers, trail_expiry)
{
    // A sample dies at TRAIL_SECONDS old (1.0 s), not before.
    mk::Trail t;
    t.update(0.5, 1.0, 1.0, NAN);
    t.update(1.4375, 2.0, 2.0, NAN); // the first is 0.9375 s old
    CHECK_EQ(t.size(), 2);
    CHECK_EQ(t.sample(1).x, 1.0);
    t.update(1.5, 3.0, 3.0, NAN); // the first is 1.0 s old: gone
    CHECK_EQ(t.size(), 2);
    CHECK_EQ(t.sample(0).x, 3.0);
    CHECK_EQ(t.sample(1).x, 2.0);

    // Everything old at once: the trail restarts with the new sample.
    mk::Trail u;
    u.update(0.5, 1.0, 1.0, NAN);
    u.update(1.5, 9.0, 9.0, NAN);
    CHECK_EQ(u.size(), 1);
    CHECK_EQ(u.sample(0).x, 9.0);
    CHECK_EQ(u.sample(0).t, 1.5);
}

TEST(markers, trail_capacity_and_ring)
{
    // Samples 0.041 s apart: 25 of them span 0.984 s, so the trail holds 25 and each next one pushes the oldest out
    // (it is then 1.025 s old). The ring wraps many times over 60 updates.
    mk::Trail t;
    for (int k = 0; k < 60; ++k)
    {
        CHECK(!t.update(k * 0.041, k, -k, NAN));
        CHECK_EQ(t.size(), k + 1 < 25 ? k + 1 : 25);
    }
    CHECK_EQ(t.size(), 25);
    for (int i = 0; i < 25; ++i)
    {
        const int k = 59 - i; // index 0 is the newest
        CHECK_EQ(t.sample(i).x, static_cast<double>(k));
        CHECK_EQ(t.sample(i).y, static_cast<double>(-k));
        CHECK_NEAR(t.sample(i).t, k * 0.041, 1e-12);
    }
}

TEST(markers, trail_snap)
{
    // snap_time is on the same clock as now (the core's), so the same snap reads the same number on every frame.
    mk::Trail t;
    // The first finite snap_time counts as a newer snap: it clears (nothing yet) and takes the sample.
    CHECK(t.update(10.0, 1.0, 1.0, 8.0));
    CHECK_EQ(t.size(), 1);
    CHECK(!t.update(10.25, 2.0, 2.0, 8.0)); // the same snap
    CHECK_EQ(t.size(), 2);
    CHECK(!t.update(10.5, 3.0, 3.0, 8.0)); // however late the frame is read: no false snap
    CHECK_EQ(t.size(), 3);
    CHECK(!t.update(10.75, 4.0, 4.0, NAN)); // no snap_time: no snap
    CHECK_EQ(t.size(), 4);

    // A newer snap (10.75, later than 8.0) clears the trail, then the sample is taken.
    CHECK(t.update(10.8, 5.0, 5.0, 10.75));
    CHECK_EQ(t.size(), 1);
    CHECK_EQ(t.sample(0).x, 5.0);
    CHECK_EQ(t.sample(0).t, 10.8);
    CHECK(!t.update(10.9, 6.0, 6.0, 10.75)); // 10.75 again
    CHECK_EQ(t.size(), 2);
    CHECK(t.update(10.95, 6.5, 6.5, 10.7500001)); // any later snap counts
    CHECK_EQ(t.size(), 1);

    // A snap older than the last one seen is not newer.
    CHECK(!t.update(11.0, 7.0, 7.0, 6.0));
    CHECK_EQ(t.size(), 2);

    // clear(): the trail empties, and the next finite snap_time counts even for the same snap.
    t.clear();
    CHECK_EQ(t.size(), 0);
    CHECK(t.update(11.1, 8.0, 8.0, 10.7500001)); // the snap seen before the clear
    CHECK_EQ(t.size(), 1);

    // NAN then finite after a fresh start: the first finite counts.
    mk::Trail u;
    CHECK(!u.update(1.0, 0.0, 0.0, NAN));
    CHECK(u.update(2.0, 0.0, 0.0, 1.0));
    CHECK_EQ(u.size(), 1);
}

TEST(markers, trail_clock_backwards)
{
    mk::Trail t;
    t.update(5.0, 1.0, 1.0, NAN);
    t.update(5.5, 2.0, 2.0, NAN);
    CHECK_EQ(t.size(), 2);
    CHECK(!t.update(4.5, 3.0, 3.0, NAN)); // now before the newest sample: cleared, no snap reported
    CHECK_EQ(t.size(), 1);
    CHECK_EQ(t.sample(0).x, 3.0);
    CHECK_EQ(t.sample(0).t, 4.5);
    CHECK(!t.update(4.75, 4.0, 4.0, NAN)); // and it carries on from the new clock
    CHECK_EQ(t.size(), 2);
}

TEST(markers, trail_size_and_opacity)
{
    CHECK_EQ(mk::trail_size(0.0), 11.0);
    CHECK_EQ(mk::trail_size(1.0), 6.0);
    CHECK_EQ(mk::trail_size(0.5), 9.0); // 8.5 rounds to a whole unit
    CHECK_EQ(mk::trail_size(0.2), 10.0); // 10
    CHECK_EQ(mk::trail_size(-1.0), 11.0); // clamped
    CHECK_EQ(mk::trail_size(7.0), 6.0);

    CHECK_NEAR(mk::trail_opacity(0.0), 0.9, 1e-12);
    CHECK_NEAR(mk::trail_opacity(1.0), 0.1, 1e-12);
    CHECK_NEAR(mk::trail_opacity(0.5), 0.5, 1e-12);
    CHECK_NEAR(mk::trail_opacity(0.25), 0.7, 1e-12);
    CHECK_NEAR(mk::trail_opacity(0.333), 0.63, 1e-12); // 0.9 - 0.2664 = 0.6336, to 0.01
    CHECK_NEAR(mk::trail_opacity(-2.0), 0.9, 1e-12);
    CHECK_NEAR(mk::trail_opacity(9.0), 0.1, 1e-12);
}

TEST(markers, state_clear)
{
    mk::State st;
    st.trail.update(1.0, 0.0, 0.0, 0.5);
    st.lift.update(4.0, 1.0, false);
    st.lag_shown = st.tick_shown = true;
    CHECK_EQ(st.trail.size(), 1);
    CHECK_EQ(st.lift.applied(), 4.0);
    st.clear();
    CHECK_EQ(st.trail.size(), 0);
    CHECK(std::isnan(st.lift.applied()));
    CHECK(!st.lag_shown);
    CHECK(!st.tick_shown);
    CHECK(st.trail.update(2.0, 0.0, 0.0, 0.5)); // the snap baseline is gone too
}

// --------------------------------------------------------------------------------------------------- the layout

TEST(markers, look_and_color_by_index)
{
    CHECK_EQ(static_cast<int>(mk::LEASH), 0);
    CHECK_EQ(static_cast<int>(mk::TRAIL), 32);
    CHECK_EQ(static_cast<int>(mk::ARROW_GAME), 57);
    CHECK_EQ(static_cast<int>(mk::ARROW_SHOWN), 58);
    CHECK_EQ(static_cast<int>(mk::LAG_LINE), 59);
    CHECK_EQ(static_cast<int>(mk::TICK), 60);
    CHECK_EQ(static_cast<int>(mk::DOT), 61);
    CHECK_EQ(static_cast<int>(mk::RING), 62);
    CHECK_EQ(static_cast<int>(mk::COUNT), 63);

    for (int i = 0; i < mk::COUNT; ++i)
    {
        const bool leash = i >= mk::LEASH && i < mk::LEASH + mk::LEASH_DOTS;
        const bool trail = i >= mk::TRAIL && i < mk::TRAIL + mk::TRAIL_DOTS;
        mk::Look look = mk::Look::Box;
        mk::Rgba color = mk::LINE;
        if (leash) look = mk::Look::Bead, color = mk::LINE;
        else if (trail) look = mk::Look::Dot, color = mk::TEAL;
        else if (i == mk::ARROW_GAME) look = mk::Look::Box, color = mk::GREY;
        else if (i == mk::ARROW_SHOWN) look = mk::Look::Box, color = mk::TEAL;
        else if (i == mk::LAG_LINE || i == mk::TICK) look = mk::Look::Box, color = mk::LINE;
        else if (i == mk::DOT) look = mk::Look::Dot, color = mk::AMBER;
        else if (i == mk::RING) look = mk::Look::Ring, color = mk::TEAL;
        CHECK(mk::look(i) == look);
        CHECK(same_color(mk::color(i), color));
    }

    // The colours themselves.
    CHECK(same_color(mk::AMBER, {0.94, 0.62, 0.15, 1.0}));
    CHECK(same_color(mk::TEAL, {0.36, 0.79, 0.65, 1.0}));
    CHECK(same_color(mk::GREY, {0.53, 0.53, 0.50, 1.0}));
    CHECK(same_color(mk::LINE, {0.95, 0.94, 0.91, 0.75}));
    CHECK(!same_color(mk::AMBER, mk::TEAL));
}

TEST(markers, line_primitive)
{
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), plain_viewport(2880, 1800, 16.0 / 9.0, 0));

    // Screen-edge to screen-edge along the horizontal midline: (0, 900) to (2880, 900).
    mk::Primitive l = mk::line(p, {100, -90, 0}, {100, 90, 0}, 4.0);
    EXPECT_PRIM(l, 1440.0, 900.0, 2880.0, 4.0, 0.0, 1.0);
    l = mk::line(p, {100, 90, 0}, {100, -90, 0}, 4.0); // reversed: turned half a turn
    EXPECT_PRIM(l, 1440.0, 900.0, 2880.0, 4.0, 180.0, 1.0);

    // Vertical, top (y 0) to bottom (y 1800): the angle is 90 deg (clockwise on screen, y down).
    l = mk::line(p, {100, 0, 56.25}, {100, 0, -56.25}, 6.0);
    EXPECT_PRIM(l, 1440.0, 900.0, 1800.0, 6.0, 90.0, 1.0);
    l = mk::line(p, {100, 0, -56.25}, {100, 0, 56.25}, 6.0);
    EXPECT_PRIM(l, 1440.0, 900.0, 1800.0, 6.0, -90.0, 1.0);

    // Corner to corner: (0, 0) to (2880, 1800), atan2(1800, 2880) = atan(0.625).
    l = mk::line(p, {100, -90, 56.25}, {100, 90, -56.25}, 4.0);
    EXPECT_PRIM(l, 1440.0, 900.0, std::hypot(2880.0, 1800.0), 4.0, std::atan(0.625) * RAD_TO_DEG, 1.0);
    l = mk::line(p, {100, 90, -56.25}, {100, -90, 56.25}, 4.0);
    EXPECT_PRIM(l, 1440.0, 900.0, std::hypot(2880.0, 1800.0), 4.0, std::atan(0.625) * RAD_TO_DEG - 180.0, 1.0);

    // Off centre: (1440, 900) to (2160, 990) -> midpoint (1800, 945). 45 right at depth 100 is +720 px (tan_x 0.9 gives
    // 90 cm of half width), 5.625 down is +90 px (half height 56.25 cm).
    l = mk::line(p, {100, 0, 0}, {100, 45, -5.625}, 4.0);
    EXPECT_PRIM(l, 1800.0, 945.0, std::hypot(720.0, 90.0), 4.0, std::atan(0.125) * RAD_TO_DEG, 1.0);

    // Hidden: both ends nearer than NEAR_CLIP, both ends on the same screen point, an end that does not project. (An
    // end behind the camera on the view axis clips to the axis at depth 10: the same screen point as the other.)
    l = mk::line(p, {100, 0, 0}, {-100, 0, 0}, 4.0);
    CHECK(!l.visible);
    l = mk::line(p, {9.9, 50, 0}, {-100, -50, 0}, 4.0);
    CHECK(!l.visible);
    l = mk::line(p, {100, 10, 0}, {100, 10, 0}, 4.0);
    CHECK(!l.visible);
    l = mk::line(p, {100, 0, 0}, {200, 0, 0}, 4.0); // both on the centre: zero length on screen
    CHECK(!l.visible);
    l = mk::line(p, {100, 0, 0}, {100, NAN, 0}, 4.0);
    CHECK(!l.visible);
    l = mk::line(mk::Projector{}, {100, 0, 0}, {100, 10, 0}, 4.0);
    CHECK(!l.visible);
    CHECK_EQ(l.w, 0.0);
    CHECK_EQ(l.opacity, 0.0);
}

TEST(markers, label_text)
{
    CHECK(mk::label_text(42) == L"42 cm");
    CHECK(mk::label_text(static_cast<int>(std::lround(42.4))) == L"42 cm");
    CHECK(mk::label_text(static_cast<int>(std::lround(42.6))) == L"43 cm");
    CHECK(mk::label_text(0) == L"0 cm");
    CHECK(mk::label_text(150) == L"150 cm");
}

TEST(markers, label_place)
{
    // "15 cm": an em of 80/3 units, a half width of 5 * 0.55 * 80/3 / 2 = 110/3, a half height of 40/3.
    const double half_w = 110.0 / 3.0, half_h = 40.0 / 3.0;
    double x = 0.0, y = 0.0;

    // The screenshot's case: the dot straight above the ring, 20 units apart. The label goes right, clear of the ring.
    mk::label_place(500.0, 400.0, 500.0, 420.0, 15, x, y);
    CHECK_NEAR(x, 500.0 + 20.0 + 6.0 + half_w, 1e-9);
    CHECK_NEAR(y, 410.0, 1e-9);
    CHECK(x - half_w >= 500.0 + 20.0); // the text box's left edge is past the ring's right edge

    // The ring straight above the dot: still to the right.
    mk::label_place(500.0, 420.0, 500.0, 400.0, 15, x, y);
    CHECK_NEAR(x, 500.0 + 26.0 + half_w, 1e-9);
    CHECK_NEAR(y, 410.0, 1e-9);

    // A level line, either way round: above.
    mk::label_place(400.0, 300.0, 600.0, 300.0, 15, x, y);
    CHECK_NEAR(x, 500.0, 1e-9);
    CHECK_NEAR(y, 300.0 - 26.0 - half_h, 1e-9);
    mk::label_place(600.0, 300.0, 400.0, 300.0, 15, x, y);
    CHECK_NEAR(x, 500.0, 1e-9);
    CHECK_NEAR(y, 300.0 - 26.0 - half_h, 1e-9);

    // The ends on top of each other: to the right.
    mk::label_place(500.0, 400.0, 500.0, 400.0, 15, x, y);
    CHECK_NEAR(x, 500.0 + 26.0 + half_w, 1e-9);
    CHECK_NEAR(y, 400.0, 1e-9);

    // A wider number moves further aside: "150 cm" is 6 glyphs.
    mk::label_place(500.0, 400.0, 500.0, 420.0, 150, x, y);
    CHECK_NEAR(x, 500.0 + 26.0 + 6.0 * 0.55 * (80.0 / 3.0) / 2.0, 1e-9);
}

TEST(markers, drawable_gates)
{
    const mk::Scene good = scene_of(live_snapshot());
    CHECK(mk::drawable(good));

    mk::Scene s = good;
    s.readable = false;
    CHECK(!mk::drawable(s));
    s = good;
    s.view.age = -0.001;
    CHECK(!mk::drawable(s));
    s = good;
    s.view.age = 0.0;
    CHECK(mk::drawable(s));
    s.view.age = 0.25; // LIVE_WINDOW itself is still live
    CHECK(mk::drawable(s));
    s.view.age = 0.2501;
    CHECK(!mk::drawable(s));
    s.view.age = 3.0;
    CHECK(!mk::drawable(s));
    s.view.age = NAN;
    CHECK(!mk::drawable(s));
    s = good;
    s.owned = true;
    CHECK(!mk::drawable(s));
    s = good;
    s.enabled = false; // switched off: a layer or the toggle-off fade still publishes a pivot and an offset
    CHECK(!mk::drawable(s));
    for (int i = 0; i < 3; ++i)
    {
        s = good;
        s.view.pivot[i] = NAN;
        CHECK(!mk::drawable(s));
        s = good;
        s.view.follow_offset[i] = NAN; // not following
        CHECK(!mk::drawable(s));
    }
    s = good;
    s.view.half_height = NAN; // a NAN half height is fine (the centre stands in)
    CHECK(mk::drawable(s));
    s = good;
    s.view.follow_yaw = NAN; // only the shown arrow needs it
    CHECK(mk::drawable(s));
}

TEST(markers, layout_hidden_when_not_drawable)
{
    const mk::Viewport vp = live_viewport();
    const mk::Scene good = scene_of(lag_snapshot({0, 40, -10}));

    auto hidden_and_cleared = [&](const mk::Scene& bad, int line) {
        mk::State st;
        mk::Layout ok = mk::layout(good, vp, st);
        if (st.trail.size() != 1 || !std::isfinite(st.lift.applied()) || !ok.shapes[mk::DOT].visible) check::fail(__FILE__, line, "primed state");
        const mk::Layout out = mk::layout(bad, vp, st);
        if (!everything_hidden(out)) check::fail(__FILE__, line, "layout hidden with lifts NAN");
        if (st.trail.size() != 0) check::fail(__FILE__, line, "trail cleared");
        if (!std::isnan(st.lift.applied())) check::fail(__FILE__, line, "lift cleared");
    };

    mk::Scene s = good;
    s.readable = false;
    hidden_and_cleared(s, __LINE__);
    s = good;
    s.view.age = -0.5;
    hidden_and_cleared(s, __LINE__);
    s = good;
    s.view.age = 0.26;
    hidden_and_cleared(s, __LINE__);
    s = good;
    s.view.age = NAN;
    hidden_and_cleared(s, __LINE__);
    s = good;
    s.owned = true;
    hidden_and_cleared(s, __LINE__);
    for (int i = 0; i < 3; ++i)
    {
        s = good;
        s.view.pivot[i] = NAN;
        hidden_and_cleared(s, __LINE__);
        s = good;
        s.view.follow_offset[i] = NAN;
        hidden_and_cleared(s, __LINE__);
    }
    s = good; // drawable, but the shown view has no projection
    s.view.shown.fov = 0.0;
    CHECK(mk::drawable(s));
    hidden_and_cleared(s, __LINE__);
    s = good; // the rendered view read, with no projection
    s.has_camera = true;
    s.camera = view_at(-400, 40, 50, 0, 0, 0, 0.0);
    hidden_and_cleared(s, __LINE__);

    // After the gate closes, the next good frame starts again with the lift landing on its target at once.
    mk::State st;
    mk::layout(good, vp, st);
    CHECK(st.lag_shown);
    CHECK(st.tick_shown);
    s = good;
    s.owned = true;
    mk::layout(s, vp, st);
    CHECK(!st.lag_shown); // the hysteresis cleared with the rest
    CHECK(!st.tick_shown);
    const mk::Layout again = mk::layout(good, vp, st);
    CHECK(again.shapes[mk::DOT].visible);
    CHECK_EQ(st.trail.size(), 1);
    CHECK_EQ(again.applied_lift, again.target_lift);
}

TEST(markers, layout_lag_frame)
{
    // Camera (-400, 40, 50) looking along +X; the shown pivot is the pivot moved by (0, 40, -10): (0, 40, 90), so lag_h
    // 40 and lag_v -10. The plane is at the feet, z = 4, at lift 0. Points: dot (0, 0, 4), ring (0, 40, -6), the ring's
    // foot (0, 40, 4).
    const cam::ViewFeed snap = lag_snapshot({0, 40, -10});
    mk::Scene scene = scene_of(snap);
    scene.feed.keep_follow = 0.2;
    mk::State st;
    const mk::Layout out = mk::layout(scene, live_viewport(), st);
    const dw::Vec3 eye{-400, 40, 50};
    CHECK_EQ(out.target_lift, 0.0);
    CHECK_EQ(out.applied_lift, 0.0);

    const Pt d = slate_x_forward(eye, {0, 0, 4});
    const Pt r = slate_x_forward(eye, {0, 40, -6});
    const Pt f = slate_x_forward(eye, {0, 40, 4});
    EXPECT_PRIM(out.shapes[mk::DOT], d.x, d.y, 20.0, 20.0, 0.0, 1.0);
    EXPECT_PRIM(out.shapes[mk::RING], r.x, r.y, 40.0, 40.0, 0.0, 0.48); // ring_opacity(0.2)

    // The tick from the ring down to its foot: 40 px up on the screen (the foot is higher), straight up: -90 deg.
    EXPECT_PRIM(out.shapes[mk::TICK], (r.x + f.x) / 2.0, (r.y + f.y) / 2.0, std::hypot(r.x - f.x, r.y - f.y), 4.0, -90.0, 1.0);
    CHECK_NEAR(out.shapes[mk::TICK].w, 40.0 / SCALE, TOL);

    // The lag line from the dot to the ring: 160 px across and 40 px down: atan(0.25).
    EXPECT_PRIM(out.shapes[mk::LAG_LINE], (d.x + r.x) / 2.0, (d.y + r.y) / 2.0, std::hypot(r.x - d.x, r.y - d.y), 4.0,
                std::atan(0.25) * RAD_TO_DEG, 1.0);
    CHECK_NEAR(out.shapes[mk::LAG_LINE].w, std::hypot(160.0, 40.0) / SCALE, TOL);

    // The label sits beside the lag line's midpoint (label_place) and reads the rounded lag. The line runs right and
    // down at 160:40, so its normal turned to the right is (40, -160) / |(160, 40)|: right and up. "40 cm" is 5 glyphs
    // at 20 pt: an em of 80/3 units, a half width of 5 * 0.55 * 80/3 / 2 = 110/3, a half height of 40/3. Away:
    // 20 (the ring's radius) + 6 + |nx| * 110/3 + |ny| * 40/3.
    CHECK(out.label.visible);
    {
        const double len = std::hypot(160.0, 40.0), nx = 40.0 / len, ny = -160.0 / len;
        const double away = 20.0 + 6.0 + nx * (110.0 / 3.0) - ny * (40.0 / 3.0);
        CHECK_NEAR(out.label.x, (d.x + r.x) / 2.0 + nx * away, TOL);
        CHECK_NEAR(out.label.y, (d.y + r.y) / 2.0 + ny * away, TOL);
    }
    CHECK_EQ(out.label.cm, 40);

    // One trail sample, the shown pivot's x, y on the plane, brand new.
    EXPECT_PRIM(out.shapes[mk::TRAIL], f.x, f.y, 11.0, 11.0, 0.0, 0.9);
    CHECK_EQ(st.trail.size(), 1);
    for (int i = 1; i < mk::TRAIL_DOTS; ++i) CHECK(!out.shapes[mk::TRAIL + i].visible);

    // No leash limit and no rotation smoothing: those stay hidden.
    for (int i = 0; i < mk::LEASH_DOTS; ++i) CHECK(!out.shapes[mk::LEASH + i].visible);
    CHECK(!out.shapes[mk::ARROW_GAME].visible);
    CHECK(!out.shapes[mk::ARROW_SHOWN].visible);
}

TEST(markers, layout_lag_thresholds)
{
    const mk::Viewport vp = live_viewport();
    auto frame = [&](dw::Vec3 offset) {
        mk::State st;
        return mk::layout(scene_of(lag_snapshot(offset)), vp, st);
    };

    // Lag line and label need a horizontal lag over 5 cm.
    mk::Layout out = frame({0, 5.0, 0});
    CHECK(!out.shapes[mk::LAG_LINE].visible);
    CHECK(!out.label.visible);
    out = frame({0, 0, 0});
    CHECK(!out.shapes[mk::LAG_LINE].visible);
    CHECK(!out.label.visible);
    out = frame({0, 6.0, 0});
    CHECK(out.shapes[mk::LAG_LINE].visible);
    CHECK(out.label.visible);
    CHECK_EQ(out.label.cm, 6);
    out = frame({0, 42.4, 0}); // rounded
    CHECK_EQ(out.label.cm, 42);
    CHECK(mk::label_text(out.label.cm) == L"42 cm");
    out = frame({0, 42.6, 0});
    CHECK_EQ(out.label.cm, 43);
    out = frame({0, -7.0, 0}); // the lag is a length: the side does not matter
    CHECK(out.shapes[mk::LAG_LINE].visible);
    CHECK_EQ(out.label.cm, 7);
    out = frame({0, 3.0, 4.0}); // lag_h uses x and y only: 3 cm, not 5
    CHECK(!out.shapes[mk::LAG_LINE].visible);

    // The vertical tick needs a vertical lag over 2 cm, either way.
    out = frame({0, 0, 2.0});
    CHECK(!out.shapes[mk::TICK].visible);
    out = frame({0, 0, -2.0});
    CHECK(!out.shapes[mk::TICK].visible);
    out = frame({0, 0, 0});
    CHECK(!out.shapes[mk::TICK].visible);
    out = frame({0, 0, 2.5});
    CHECK(out.shapes[mk::TICK].visible);
    out = frame({0, 0, -2.5});
    CHECK(out.shapes[mk::TICK].visible);
    CHECK(!out.shapes[mk::LAG_LINE].visible); // no horizontal lag: the tick alone
}

TEST(markers, layout_leash_ring)
{
    // Leash radius max_lag_h * keep_follow = 85 on the plane round the dot (0, 0, 4). Camera (-400, 40, 50).
    mk::Scene scene = scene_of(lag_snapshot({0, 40, -10}));
    scene.max_lag_h = 85.0;
    scene.feed.keep_follow = 1.0;
    mk::State st;
    mk::Layout out = mk::layout(scene, live_viewport(), st);
    const dw::Vec3 eye{-400, 40, 50};
    CHECK_EQ(out.applied_lift, 0.0);
    for (int i = 0; i < mk::LEASH_DOTS; ++i) CHECK(out.shapes[mk::LEASH + i].visible);
    Pt e = slate_x_forward(eye, {85, 0, 4}); // dot 0: +X, away from the camera
    EXPECT_PRIM(out.shapes[mk::LEASH + 0], e.x, e.y, 10.0, 10.0, 0.0, 1.0);
    e = slate_x_forward(eye, {0, 85, 4}); // dot 8: +Y
    EXPECT_PRIM(out.shapes[mk::LEASH + 8], e.x, e.y, 10.0, 10.0, 0.0, 1.0);
    e = slate_x_forward(eye, {-85, 0, 4}); // dot 16: -X, towards the camera
    EXPECT_PRIM(out.shapes[mk::LEASH + 16], e.x, e.y, 10.0, 10.0, 0.0, 1.0);
    e = slate_x_forward(eye, {0, -85, 4}); // dot 24: -Y
    EXPECT_PRIM(out.shapes[mk::LEASH + 24], e.x, e.y, 10.0, 10.0, 0.0, 1.0);
    EXPECT_PRIM(out.shapes[mk::RING], slate_x_forward(eye, {0, 40, -6}).x, slate_x_forward(eye, {0, 40, -6}).y, 40.0, 40.0, 0.0, 1.0);

    // Under MIN_LEASH (0.9 cm) or with max_lag_h 0 the leash is not drawn.
    scene.feed.keep_follow = 0.01;
    st.clear();
    out = mk::layout(scene, live_viewport(), st);
    for (int i = 0; i < mk::LEASH_DOTS; ++i) CHECK(!out.shapes[mk::LEASH + i].visible);
    scene.feed.keep_follow = 1.0;
    scene.max_lag_h = 0.0;
    st.clear();
    out = mk::layout(scene, live_viewport(), st);
    for (int i = 0; i < mk::LEASH_DOTS; ++i) CHECK(!out.shapes[mk::LEASH + i].visible);
}

TEST(markers, layout_arrows)
{
    // The shown camera orbits the pivot: at (0, -400, 60) facing +Y (yaw 90) it sees the pivot as the game camera at
    // (-400, 0, 60) facing +X did, so the follow has no lag (offset 0), the plane at the feet (z = 4, no lift). The
    // game's yaw is 0 (arrow along +X), the follow's yaw 90 (arrow along +Y). The camera looks along +Y: depth is
    // y + 400, and world +X is to its left.
    cam::ViewFeed snap = snapshot_of(view_at(-400, 0, 60), view_at(0, -400, 60, 0, 90, 0), {0, 0, 100}, 96);
    for (double& o : snap.follow_offset) o = 0.0;
    mk::Scene scene = scene_of(snap);
    scene.rotation_smoothing = true;
    scene.feed.keep_turn = 0.5;
    mk::State st;
    const mk::Layout out = mk::layout(scene, live_viewport(), st);
    const dw::Vec3 eye{0, -400, 60};
    CHECK_EQ(out.applied_lift, 0.0);

    const Pt d = slate_y_forward(eye, {0, 0, 4});
    EXPECT_PRIM(out.shapes[mk::DOT], d.x, d.y, 20.0, 20.0, 0.0, 1.0);
    CHECK_NEAR(d.x, 1440.0 / SCALE, TOL);
    const Pt g = slate_y_forward(eye, {60, 0, 4}); // ARROW_LENGTH along the game's yaw
    EXPECT_PRIM(out.shapes[mk::ARROW_GAME], (d.x + g.x) / 2.0, (d.y + g.y) / 2.0, std::hypot(g.x - d.x, g.y - d.y), 4.0,
                std::atan2(g.y - d.y, g.x - d.x) * RAD_TO_DEG, 1.0);
    CHECK_NEAR(g.x, 1440.0 * (1.0 - 60.0 / 360.0) / SCALE, TOL); // 60 cm to the camera's left
    const Pt sh = slate_y_forward(eye, {0, 60, 4}); // along the shown yaw
    EXPECT_PRIM(out.shapes[mk::ARROW_SHOWN], (d.x + sh.x) / 2.0, (d.y + sh.y) / 2.0, std::hypot(sh.x - d.x, sh.y - d.y), 4.0,
                std::atan2(sh.y - d.y, sh.x - d.x) * RAD_TO_DEG, 1.0);
    CHECK_NEAR(out.shapes[mk::ARROW_SHOWN].angle_deg, -90.0, 1e-6); // straight up the screen: away from the camera
    CHECK_NEAR(out.shapes[mk::ARROW_GAME].angle_deg, 180.0, 1e-6);  // to the left of the screen

    // No lag: no lag line, label or tick.
    CHECK(!out.shapes[mk::LAG_LINE].visible);
    CHECK(!out.shapes[mk::TICK].visible);
    CHECK(!out.label.visible);

    // Each gate alone hides both arrows.
    auto arrows = [&](bool smoothing, double keep_turn) {
        mk::Scene s = scene;
        s.rotation_smoothing = smoothing;
        s.feed.keep_turn = keep_turn;
        mk::State fresh;
        const mk::Layout l = mk::layout(s, live_viewport(), fresh);
        return l.shapes[mk::ARROW_GAME].visible && l.shapes[mk::ARROW_SHOWN].visible;
    };
    auto no_arrows = [&](bool smoothing, double keep_turn) {
        mk::Scene s = scene;
        s.rotation_smoothing = smoothing;
        s.feed.keep_turn = keep_turn;
        mk::State fresh;
        const mk::Layout l = mk::layout(s, live_viewport(), fresh);
        return !l.shapes[mk::ARROW_GAME].visible && !l.shapes[mk::ARROW_SHOWN].visible;
    };
    CHECK(arrows(true, 0.5));
    CHECK(arrows(true, 0.01));
    CHECK(no_arrows(false, 0.5));  // rotation smoothing off
    CHECK(no_arrows(true, NAN));   // keep_turn unknown
    CHECK(no_arrows(true, 0.0));   // not turning
    CHECK(no_arrows(true, -0.5));
    CHECK(no_arrows(true, INFINITY));
}

TEST(markers, layout_hides_points_that_do_not_project)
{
    // Camera (-30, 0, 100) facing +X, 30 cm from the pivot. A leash of 85 has dots at x = 85 cos(a); those less than
    // MIN_DEPTH (1 cm) in front of the camera, x + 30 < 1, do not project and are hidden alone; the others draw.
    const cam::ViewFeed snap = snapshot_of(view_at(-30, 0, 100), view_at(-30, 0, 100), {0, 0, 100}, 96);
    mk::Scene scene = scene_of(snap);
    scene.max_lag_h = 85.0;
    scene.feed.keep_follow = 1.0;
    mk::State st;
    const mk::Layout out = mk::layout(scene, live_viewport(), st);
    CHECK(out.shapes[mk::DOT].visible);
    int visible = 0, hidden = 0;
    for (int i = 0; i < mk::LEASH_DOTS; ++i)
    {
        const double x = 85.0 * std::cos(2.0 * dw::PI * i / mk::LEASH_DOTS);
        const bool projects = x + 30.0 >= 1.0;
        CHECK_EQ(out.shapes[mk::LEASH + i].visible, projects);
        (projects ? visible : hidden)++;
    }
    CHECK(visible > 0 && hidden > 0);
}

TEST(markers, layout_lift_and_snap)
{
    // The live scene: the plane rises to the target on the first frame, eases on later ones, lands at once on a snap.
    const mk::Viewport vp = live_viewport();
    mk::Scene scene = scene_of(live_snapshot());
    scene.max_lag_h = 85.0;
    scene.feed.keep_follow = 1.0;
    mk::State st;
    mk::Layout out = mk::layout(scene, vp, st);
    const double leash_edge = 146.0 - 0.84 * 117.0 * 0.5625;
    CHECK(out.target_lift >= leash_edge);
    CHECK(out.target_lift <= leash_edge + 106.0 / 1024.0);
    CHECK_EQ(out.applied_lift, out.target_lift); // the first frame lands on the target
    const double first = out.applied_lift;
    // The dot sits on the plane at the feet plus the lift: z = 4 + lift, 202 ahead of the camera at (-202, 0, 150).
    Pt d = slate_x_forward({-202, 0, 150}, {0, 0, 4 + first});
    EXPECT_PRIM(out.shapes[mk::DOT], d.x, d.y, 20.0, 20.0, 0.0, 1.0);
    CHECK(out.shapes[mk::DOT].y * SCALE <= 0.92 * H + 1e-6); // and above the fit line
    for (int i = 0; i < mk::LEASH_DOTS; ++i) CHECK(out.shapes[mk::LEASH + i].y * SCALE <= 0.92 * H + 1e-6);

    // The camera drops to (-400, 0, 60): the target is 0 and the applied lift eases 1 - exp(-6 dt) of the way.
    set_view(scene, snapshot_of(view_at(-400, 0, 60), view_at(-400, 0, 60), {0, 0, 100}, 96));
    scene.view.now = 100.1;
    out = mk::layout(scene, vp, st);
    CHECK_EQ(out.target_lift, 0.0);
    CHECK_NEAR(out.applied_lift, first * std::exp(-0.6), 1e-9);
    CHECK_EQ(st.trail.size(), 2);

    // A newer snap: the trail restarts and the lift lands on its target at once.
    set_view(scene, snapshot_of(view_at(-202, 0, 150), view_at(-202, 0, 150), {0, 0, 100}, 96));
    scene.view.now = 100.2;
    out = mk::layout(scene, vp, st); // eases up from a small lift
    CHECK(out.applied_lift < out.target_lift);
    scene.view.now = 100.3;
    scene.feed.snap_time = 100.25;
    out = mk::layout(scene, vp, st);
    CHECK_EQ(out.applied_lift, out.target_lift);
    CHECK_EQ(st.trail.size(), 1);
}

TEST(markers, layout_trail_ages)
{
    // A trail of three frames 0.5 s apart at a fixed camera: the samples take their size and opacity from their age.
    const mk::Viewport vp = live_viewport();
    mk::Scene scene = scene_of(lag_snapshot({0, 40, -10}));
    mk::State st;
    mk::Layout out;
    for (int k = 0; k < 3; ++k)
    {
        set_view(scene, lag_snapshot({0, 40.0 + 10.0 * k, -10})); // the followed pivot moves along +Y, 10 cm a frame
        scene.view.now = 100.0 + 0.25 * k;
        out = mk::layout(scene, vp, st);
    }
    CHECK_EQ(st.trail.size(), 3);
    // Ages 0, 0.25, 0.5 s: sizes 11, 10 (9.75 rounds up), 9 (8.5 rounds up); opacity 0.9, 0.7, 0.5.
    CHECK_EQ(out.shapes[mk::TRAIL + 0].w, 11.0);
    CHECK_EQ(out.shapes[mk::TRAIL + 1].w, 10.0);
    CHECK_EQ(out.shapes[mk::TRAIL + 2].w, 9.0);
    CHECK_NEAR(out.shapes[mk::TRAIL + 0].opacity, 0.9, 1e-12);
    CHECK_NEAR(out.shapes[mk::TRAIL + 1].opacity, 0.7, 1e-12);
    CHECK_NEAR(out.shapes[mk::TRAIL + 2].opacity, 0.5, 1e-12);
    // Each on the plane (z = 4) at the shown pivot it had: y = 40, 50, 60 seen from the current camera (-400, 60, 50).
    const dw::Vec3 eye{-400, 60, 50};
    for (int i = 0; i < 3; ++i)
    {
        const Pt e = slate_x_forward(eye, {0, 60.0 - 10.0 * i, 4});
        CHECK_NEAR(out.shapes[mk::TRAIL + i].x, e.x, TOL);
        CHECK_NEAR(out.shapes[mk::TRAIL + i].y, e.y, TOL);
    }
    CHECK(!out.shapes[mk::TRAIL + 3].visible);
}

// ------------------------------------------------------------------------------ near clipping, hysteresis, views

TEST(markers, clip_near)
{
    // Camera at the origin facing +X: depth is x.
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), plain_viewport(2880, 1800, 16.0 / 9.0, 0));
    dw::Vec3 a{100, 50, 0}, b{-100, -50, 0};
    CHECK(mk::clip_near(p, a, b)); // b moves 110/200 of the way to a: depth 10
    CHECK_EQ(a.x, 100.0);
    CHECK_EQ(a.y, 50.0);
    CHECK_NEAR(b.x, 10.0, 1e-12);
    CHECK_NEAR(b.y, 5.0, 1e-12);
    CHECK_NEAR(b.z, 0.0, 1e-12);

    a = {-100, -50, 0}; // the other end nearer
    b = {100, 50, 0};
    CHECK(mk::clip_near(p, a, b));
    CHECK_NEAR(a.x, 10.0, 1e-12);
    CHECK_NEAR(a.y, 5.0, 1e-12);
    CHECK_EQ(b.x, 100.0);

    a = {10, 1, 2}; // exactly NEAR_CLIP is not moved
    b = {50, 3, 4};
    CHECK(mk::clip_near(p, a, b));
    CHECK_EQ(a.x, 10.0);
    CHECK_EQ(a.y, 1.0);
    CHECK_EQ(b.x, 50.0);

    a = {9.9, 1, 2}; // both nearer: false, untouched
    b = {-50, 3, 4};
    CHECK(!mk::clip_near(p, a, b));
    CHECK_EQ(a.x, 9.9);
    CHECK_EQ(b.x, -50.0);

    a = {100, NAN, 0};
    b = {-100, 0, 0};
    CHECK(!mk::clip_near(p, a, b));
    CHECK_EQ(b.x, -100.0);
    a = {100, 0, 0};
    CHECK(!mk::clip_near(mk::Projector{}, a, b));
    CHECK_NEAR(mk::depth(p, {37, 5, -8}), 37.0, 1e-12);
    CHECK(std::isnan(mk::depth(mk::Projector{}, {37, 5, -8})));
}

TEST(markers, line_clipped_to_the_near_plane)
{
    const mk::Projector p = mk::make_projector(view_at(0, 0, 0), plain_viewport(2880, 1800, 16.0 / 9.0, 0));
    // (100, 50, 0) to (-100, 150, 0): the far end is behind the camera and clips to (10, 95, 0). Projected: x = 1440
    // (1 + 50 / 90) = 2240 and 1440 (1 + 95 / 9) = 16640, both on the midline.
    const mk::Primitive l = mk::line(p, {100, 50, 0}, {-100, 150, 0}, 4.0);
    EXPECT_PRIM(l, (2240.0 + 16640.0) / 2.0, 900.0, 16640.0 - 2240.0, 4.0, 0.0, 1.0);
    // The same line reversed: the clip works from either end.
    const mk::Primitive r = mk::line(p, {-100, 150, 0}, {100, 50, 0}, 4.0);
    EXPECT_PRIM(r, (2240.0 + 16640.0) / 2.0, 900.0, 16640.0 - 2240.0, 4.0, 180.0, 1.0);
}

TEST(markers, hysteresis)
{
    CHECK(mk::hysteresis(false, 5.1, 5.0, 4.0));
    CHECK(!mk::hysteresis(false, 5.0, 5.0, 4.0)); // over, not at
    CHECK(!mk::hysteresis(false, 4.5, 5.0, 4.0)); // in the band: as it was
    CHECK(mk::hysteresis(true, 4.5, 5.0, 4.0));
    CHECK(mk::hysteresis(true, 4.0, 5.0, 4.0)); // under, not at
    CHECK(!mk::hysteresis(true, 3.9, 5.0, 4.0));
    CHECK(!mk::hysteresis(true, NAN, 5.0, 4.0));
    CHECK(!mk::hysteresis(true, INFINITY, 5.0, 4.0));
}

TEST(markers, layout_lag_hysteresis)
{
    // One State over several frames: the lag line and label show over 5 cm and hide under 4 cm; the tick shows over
    // 2 cm and hides under 1.5 cm.
    const mk::Viewport vp = live_viewport();
    mk::State st;
    auto frame = [&](dw::Vec3 offset) { return mk::layout(scene_of(lag_snapshot(offset)), vp, st); };

    mk::Layout out = frame({0, 4.5, 0});
    CHECK(!out.shapes[mk::LAG_LINE].visible); // not shown yet: the band keeps it hidden
    CHECK(!out.label.visible);
    out = frame({0, 6.0, 0});
    CHECK(out.shapes[mk::LAG_LINE].visible);
    CHECK(out.label.visible);
    out = frame({0, 4.5, 0}); // in the band: stays
    CHECK(out.shapes[mk::LAG_LINE].visible);
    CHECK(out.label.visible);
    CHECK_EQ(out.label.cm, 5); // 4.5 rounds away from zero
    out = frame({0, 4.0, 0});
    CHECK(out.shapes[mk::LAG_LINE].visible);
    out = frame({0, 3.9, 0});
    CHECK(!out.shapes[mk::LAG_LINE].visible);
    CHECK(!out.label.visible);
    out = frame({0, 4.9, 0});
    CHECK(!out.shapes[mk::LAG_LINE].visible);
    out = frame({0, 5.1, 0});
    CHECK(out.shapes[mk::LAG_LINE].visible);

    out = frame({0, 0, 1.9});
    CHECK(!out.shapes[mk::TICK].visible);
    CHECK(!out.shapes[mk::LAG_LINE].visible); // no horizontal lag now
    out = frame({0, 0, -2.1});
    CHECK(out.shapes[mk::TICK].visible);
    out = frame({0, 0, 1.6});
    CHECK(out.shapes[mk::TICK].visible);
    out = frame({0, 0, -1.5}); // under, not at
    CHECK(out.shapes[mk::TICK].visible);
    out = frame({0, 0, 1.4});
    CHECK(!out.shapes[mk::TICK].visible);
    out = frame({0, 0, 1.9});
    CHECK(!out.shapes[mk::TICK].visible);
}

TEST(markers, layout_projects_the_rendered_view)
{
    // The feed's shown view is at (-400, 40, 50); the rendered camera (a shake) at (-400, 40, 70), turned 2 deg in yaw.
    mk::Scene scene = scene_of(lag_snapshot({0, 40, -10}));
    CHECK(&mk::projection_view(scene) == &scene.view.shown);
    scene.has_camera = true;
    scene.camera = view_at(-400, 40, 70, 0, 2, 0);
    CHECK(&mk::projection_view(scene) == &scene.camera);
    mk::State st;
    const mk::Layout out = mk::layout(scene, live_viewport(), st);
    const mk::Projector p = mk::make_projector(scene.camera, live_viewport());
    double x = 0.0, y = 0.0;
    CHECK(mk::project(p, {0, 0, 4}, x, y));
    EXPECT_PRIM(out.shapes[mk::DOT], x, y, 20.0, 20.0, 0.0, 1.0);
    CHECK(mk::project(p, {0, 40, -6}, x, y)); // world positions still from the feed
    EXPECT_PRIM(out.shapes[mk::RING], x, y, 40.0, 40.0, 0.0, 1.0);

    // max_lift is measured from the rendered camera: 20 cm from the pivot at z 120, nothing fits, so the target is
    // the ceiling 120 - 40 - 4 = 76 (the shown view at z 150 would give 106).
    mk::Scene close = scene_of(snapshot_of(view_at(-20, 0, 150), view_at(-20, 0, 150), {0, 0, 100}, 96));
    close.has_camera = true;
    close.camera = view_at(-20, 0, 120);
    mk::State cs;
    CHECK_EQ(mk::layout(close, live_viewport(), cs).target_lift, 76.0);
    close.has_camera = false;
    cs.clear();
    CHECK_EQ(mk::layout(close, live_viewport(), cs).target_lift, 106.0);
}

TEST(markers, layout_ignores_layers_in_the_shown_view)
{
    // Another mod's layer lifted the shown view 30 cm and turned it 45 deg; the follow itself has no lag and faces
    // yaw 90. The ring sits on the dot, no lag line or tick shows, and the shown arrow follows follow_yaw.
    cam::ViewFeed v = snapshot_of(view_at(-400, 0, 60), view_at(-400, 0, 90, 0, 45, 0), {0, 0, 100}, 96);
    for (double& o : v.follow_offset) o = 0.0;
    v.follow_yaw = 90.0;
    mk::Scene scene = scene_of(v);
    scene.has_camera = true;
    scene.camera = view_at(-400, 0, 60); // the rendered view, level along +X
    scene.rotation_smoothing = true;
    scene.feed.keep_turn = 1.0;
    mk::State st;
    const mk::Layout out = mk::layout(scene, live_viewport(), st);
    const dw::Vec3 eye{-400, 0, 60};
    const Pt d = slate_x_forward(eye, {0, 0, 4});
    EXPECT_PRIM(out.shapes[mk::RING], d.x, d.y, 40.0, 40.0, 0.0, 1.0);
    CHECK(!out.shapes[mk::LAG_LINE].visible);
    CHECK(!out.shapes[mk::TICK].visible);
    const Pt s = slate_x_forward(eye, {0, 60, 4}); // along +Y: to the right on screen
    EXPECT_PRIM(out.shapes[mk::ARROW_SHOWN], (d.x + s.x) / 2.0, (d.y + s.y) / 2.0, s.x - d.x, 4.0, 0.0, 1.0);
    const Pt g = slate_x_forward(eye, {60, 0, 4}); // the game's yaw 0: along +X, away from the camera
    EXPECT_PRIM(out.shapes[mk::ARROW_GAME], (d.x + g.x) / 2.0, (d.y + g.y) / 2.0, d.y - g.y, 4.0, -90.0, 1.0);

    // follow_yaw unknown: the shown arrow alone hides.
    scene.view.follow_yaw = NAN;
    st.clear();
    const mk::Layout no_yaw = mk::layout(scene, live_viewport(), st);
    CHECK(!no_yaw.shapes[mk::ARROW_SHOWN].visible);
    CHECK(no_yaw.shapes[mk::ARROW_GAME].visible);
}

TEST(markers, layout_hidden_on_a_tiny_viewport)
{
    // GetViewportSize's 1x1 without a game viewport: everything hidden and the state cleared.
    mk::Viewport vp = live_viewport();
    vp.width = vp.height = 1.0;
    vp.scale = 1.0;
    mk::State st;
    mk::layout(scene_of(lag_snapshot({0, 40, -10})), live_viewport(), st);
    CHECK_EQ(st.trail.size(), 1);
    CHECK(everything_hidden(mk::layout(scene_of(lag_snapshot({0, 40, -10})), vp, st)));
    CHECK_EQ(st.trail.size(), 0);
}
