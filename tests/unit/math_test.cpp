// common/math.hpp: rotator and quaternion conversions as UE does them, rotation, slerp, angle_between.

#include "check.hpp"
#include "common/math.hpp"

namespace
{
    auto same_rotation(const dw::Quat& a, const dw::Quat& b) -> bool { return dw::angle_between(a, b) < 1e-6; }
} // namespace

TEST(math, rotator_round_trip)
{
    const double cases[][3] = {{0, 0, 0}, {10, 20, 30}, {-45, 170, -10}, {80, -120, 5}, {-89, 359, 0}};
    for (auto& c : cases)
    {
        double p = 0, y = 0, r = 0;
        dw::to_rotator(dw::from_rotator(c[0], c[1], c[2]), p, y, r);
        CHECK_NEAR(p, c[0], 1e-9);
        CHECK_NEAR(dw::normalize_axis(y - c[1]), 0.0, 1e-9);
        CHECK_NEAR(r, c[2], 1e-9);
    }
}

TEST(math, rotator_gimbal_pitch)
{
    double p = 0, y = 0, r = 0;
    dw::to_rotator(dw::from_rotator(90, 30, 0), p, y, r);
    CHECK_EQ(p, 90.0);
    dw::to_rotator(dw::from_rotator(-90, 30, 0), p, y, r);
    CHECK_EQ(p, -90.0);
}

TEST(math, yaw_turns_forward_to_right)
{
    // UE: X forward, Y right, Z up; yaw 90 turns X onto Y, pitch 90 onto Z.
    dw::Vec3 v = dw::rotate(dw::from_rotator(0, 90, 0), {1, 0, 0});
    CHECK_NEAR(v.x, 0.0, 1e-12);
    CHECK_NEAR(v.y, 1.0, 1e-12);
    CHECK_NEAR(v.z, 0.0, 1e-12);
    v = dw::rotate(dw::from_rotator(90, 0, 0), {1, 0, 0});
    CHECK_NEAR(v.z, 1.0, 1e-12);
}

TEST(math, normalize_axis)
{
    CHECK_EQ(dw::normalize_axis(270.0), -90.0);
    CHECK_EQ(dw::normalize_axis(-190.0), 170.0);
    CHECK_EQ(dw::normalize_axis(180.0), 180.0);
    CHECK_EQ(dw::normalize_axis(720.0), 0.0);
}

TEST(math, normalize_degenerate_is_identity)
{
    dw::Quat q = dw::normalize({0, 0, 0, 0});
    CHECK_EQ(q.w, 1.0);
    CHECK_EQ(q.x, 0.0);
    q = dw::normalize({0, 0, 0, 2});
    CHECK_EQ(q.w, 1.0);
}

TEST(math, slerp_endpoints_and_middle)
{
    dw::Quat a = dw::from_rotator(0, 0, 0), b = dw::from_rotator(0, 90, 0);
    CHECK(same_rotation(dw::slerp(a, b, 0.0), a));
    CHECK(same_rotation(dw::slerp(a, b, 1.0), b));
    CHECK(same_rotation(dw::slerp(a, b, 0.5), dw::from_rotator(0, 45, 0)));
    // The short way round, whichever sign b carries.
    dw::Quat nb{-b.x, -b.y, -b.z, -b.w};
    CHECK(same_rotation(dw::slerp(a, nb, 0.5), dw::from_rotator(0, 45, 0)));
}

TEST(math, angle_between_ignores_sign)
{
    dw::Quat a = dw::from_rotator(0, 30, 0), b = dw::from_rotator(0, 100, 0);
    CHECK_NEAR(dw::angle_between(a, b), 70.0 * dw::PI / 180.0, 1e-9);
    CHECK_NEAR(dw::angle_between(a, {-a.x, -a.y, -a.z, -a.w}), 0.0, 1e-6);
}

TEST(math, vector_ops)
{
    dw::Vec3 a{1, 2, 3}, b{4, 6, 8};
    dw::Vec3 d = b - a;
    CHECK_EQ(dw::length(d), std::sqrt(9.0 + 16.0 + 25.0));
    dw::Vec3 s = (a + b) * 2.0;
    CHECK_EQ(s.x, 10.0);
    CHECK_EQ(s.z, 22.0);
}
