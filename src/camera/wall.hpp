// Keeping a moved camera in front of a wall the game already pulled its own camera in for. Used by the follow
// (follow/follow.hpp) on its result and by the core's crossfade on the faded part of the lag. Numbers only.
#pragma once

#include "../common/math.hpp"

#include <algorithm>

namespace dwcam
{
    // How much of the wall clamp applies: 0 at 0.85 of the usual distance, 1 at 0.65 and closer. Eased because the
    // game's own modes (aiming, close combat) cross 0.85 too, and a hard threshold dropped the whole lag in one frame.
    inline auto wall_weight(double game_distance, double nominal_distance) -> double
    {
        if (!(nominal_distance > 0.0)) return 0.0;
        double x = std::clamp((0.85 - game_distance / nominal_distance) / 0.20, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    // Pulls result toward the game's distance from the pivot by weight; true if it moved.
    inline auto clamp_to_wall(dwsc::Vec3& result, const dwsc::Vec3& pivot, double game_distance, double weight) -> bool
    {
        if (weight <= 0.0) return false;
        dwsc::Vec3 out = result - pivot;
        double out_distance = dwsc::length(out);
        if (!(out_distance > game_distance) || out_distance <= 0.0) return false;
        double limit = out_distance + (game_distance - out_distance) * weight;
        result = pivot + out * (limit / out_distance);
        return true;
    }
} // namespace dwcam
