// The follow's catch-up curves: the rate at a given lag, and its frame-rate independent share per frame. Numbers only.
#pragma once

#include <algorithm>
#include <cmath>

namespace dw::smoothwalker::follow
{
    // Catch-up strength as the lag grows: 0 constant, 1 linear, 2 smoothstep, 3 cubic ease in-out. x in [0, 1].
    inline double curve(int kind, double x)
    {
        x = std::clamp(x, 0.0, 1.0);
        switch (kind)
        {
        case 1: return x;
        case 2: return x * x * (3.0 - 2.0 * x);
        case 3: return x < 0.5 ? 4.0 * x * x * x : 1.0 - std::pow(-2.0 * x + 2.0, 3.0) / 2.0;
        default: return 1.0;
        }
    }

    // The catch-up rate in effect at this lag, 1/s: the rate scaled by the curve.
    inline double follow_rate(double rate, int kind, double lag, double catchup, double floor_scale)
    {
        double scale = kind == 0 ? 1.0 : floor_scale + (1.0 - floor_scale) * curve(kind, catchup > 0.0 ? lag / catchup : 1.0);
        return std::max(rate, 0.0) * scale;
    }

    // Frame-rate independent: the same lag closes at the same speed at any frame time.
    inline double follow_alpha(double rate, int kind, double lag, double catchup, double floor_scale, double dt)
    {
        return 1.0 - std::exp(-follow_rate(rate, kind, lag, catchup, floor_scale) * dt);
    }
} // namespace dw::smoothwalker::follow
