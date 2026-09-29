// camera/wall.hpp: how much of the wall clamp applies, and the clamp itself.

#include "check.hpp"
#include "camera/wall.hpp"

#include <cmath>

using dw::camera::clamp_to_wall;
using dw::camera::wall_weight;

TEST(wall, weight_edges)
{
    CHECK_EQ(wall_weight(85.0, 100.0), 0.0);  // 0.85 of the usual distance: none
    CHECK_EQ(wall_weight(90.0, 100.0), 0.0);
    CHECK_EQ(wall_weight(120.0, 100.0), 0.0);
    CHECK_NEAR(wall_weight(65.0, 100.0), 1.0, 1e-12); // 0.65 and closer: all
    CHECK_EQ(wall_weight(10.0, 100.0), 1.0);
    CHECK_NEAR(wall_weight(75.0, 100.0), 0.5, 1e-12); // smoothstep between
    CHECK_NEAR(wall_weight(80.0, 100.0), 0.25 * 0.25 * 2.5, 1e-12);
}

TEST(wall, weight_without_a_distance)
{
    CHECK_EQ(wall_weight(50.0, 0.0), 0.0);
    CHECK_EQ(wall_weight(50.0, -1.0), 0.0);
    CHECK_EQ(wall_weight(50.0, NAN), 0.0);
}

TEST(wall, clamp)
{
    const dw::Vec3 pivot{0, 0, 0};
    dw::Vec3 r{300, 0, 0};
    CHECK(!clamp_to_wall(r, pivot, 200.0, 0.0)); // no weight
    CHECK_EQ(r.x, 300.0);
    CHECK(clamp_to_wall(r, pivot, 200.0, 1.0)); // all the way to the game's distance
    CHECK_NEAR(r.x, 200.0, 1e-12);
    r = {300, 0, 0};
    CHECK(clamp_to_wall(r, pivot, 200.0, 0.5)); // half way, along the same direction
    CHECK_NEAR(r.x, 250.0, 1e-12);
    CHECK_EQ(r.y, 0.0);
    r = {150, 0, 0};
    CHECK(!clamp_to_wall(r, pivot, 200.0, 1.0)); // already inside the game's distance
    CHECK_EQ(r.x, 150.0);
    r = {0, 0, 0};
    CHECK(!clamp_to_wall(r, pivot, 0.0, 1.0)); // at the pivot
    r = {3, 4, 0};
    CHECK(clamp_to_wall(r, {0, 0, 0}, 2.5, 1.0));
    CHECK_NEAR(r.x, 1.5, 1e-12);
    CHECK_NEAR(r.y, 2.0, 1e-12);
}
