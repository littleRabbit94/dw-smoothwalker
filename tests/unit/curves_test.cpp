// smoothwalker/follow/curves.hpp: the catch-up curves, the rate at a lag, the per-frame share.

#include "check.hpp"
#include "smoothwalker/follow/curves.hpp"

#include <cmath>

using namespace dw::smoothwalker::follow;

TEST(curves, shapes)
{
    for (int kind = 0; kind <= 3; ++kind)
    {
        CHECK_EQ(curve(kind, 1.0), 1.0);
        CHECK_EQ(curve(kind, 2.0), 1.0); // clamped
    }
    CHECK_EQ(curve(0, 0.0), 1.0);
    CHECK_EQ(curve(1, 0.0), 0.0);
    CHECK_EQ(curve(2, 0.0), 0.0);
    CHECK_EQ(curve(3, 0.0), 0.0);
    CHECK_EQ(curve(1, 0.25), 0.25);
    CHECK_EQ(curve(2, 0.5), 0.5);
    CHECK_EQ(curve(2, 0.25), 0.25 * 0.25 * 2.5);
    CHECK_EQ(curve(3, 0.25), 4.0 * 0.25 * 0.25 * 0.25);
    CHECK_NEAR(curve(3, 0.75), 1.0 - std::pow(0.5, 3.0) / 2.0, 1e-15);
    CHECK_EQ(curve(1, -1.0), 0.0);
    CHECK_EQ(curve(7, 0.3), 1.0); // unknown kinds are constant
}

TEST(curves, rate_at_lag)
{
    CHECK_EQ(follow_rate(6.5, 0, 0.0, 150.0, 0.35), 6.5); // constant: the floor does not apply
    CHECK_EQ(follow_rate(6.5, 2, 0.0, 150.0, 0.35), 6.5 * 0.35);
    CHECK_EQ(follow_rate(6.5, 2, 150.0, 150.0, 0.35), 6.5);
    CHECK_EQ(follow_rate(6.5, 1, 75.0, 150.0, 0.35), 6.5 * (0.35 + 0.65 * 0.5));
    CHECK_EQ(follow_rate(6.5, 1, 10.0, 0.0, 0.35), 6.5); // no catch-up distance: full rate
    CHECK_EQ(follow_rate(-3.0, 0, 10.0, 150.0, 0.35), 0.0);
}

TEST(curves, alpha)
{
    CHECK_EQ(follow_alpha(10.0, 0, 0.0, 150.0, 0.35, 0.0), 0.0);
    CHECK_NEAR(follow_alpha(10.0, 0, 0.0, 150.0, 0.35, 0.1), 1.0 - std::exp(-1.0), 1e-15);
}

// Two half steps close the same share of a lag as one full step: 1 - (1 - a(dt/2))^2 == a(dt), for every curve at a
// fixed lag, so the follow runs at the same speed at any frame time.
TEST(curves, frame_rate_independent)
{
    for (int kind = 0; kind <= 3; ++kind)
    {
        for (double lag : {0.0, 20.0, 90.0, 400.0})
        {
            for (double dt : {1.0 / 30.0, 1.0 / 60.0, 1.0 / 144.0})
            {
                double full = follow_alpha(6.5, kind, lag, 150.0, 0.35, dt);
                double half = follow_alpha(6.5, kind, lag, 150.0, 0.35, dt / 2.0);
                CHECK_NEAR(1.0 - (1.0 - half) * (1.0 - half), full, 1e-12);
            }
        }
    }
}
