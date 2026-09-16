// Smoothing math with no Unreal or UE4SS types, safe on the worker threads that call GetCameraView.
#pragma once

#include <algorithm>
#include <cmath>

namespace dwsc
{
    struct Vec3
    {
        double x{}, y{}, z{};
    };

    inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    inline double length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

    struct Quat
    {
        double x{}, y{}, z{}, w{1.0};
    };

    constexpr double PI = 3.14159265358979323846;

    // UE FRotator::Quaternion, degrees in.
    inline Quat from_rotator(double pitch, double yaw, double roll)
    {
        const double h = PI / 360.0;
        const double sr = std::sin(roll * h), cr = std::cos(roll * h);
        const double sp = std::sin(pitch * h), cp = std::cos(pitch * h);
        const double sy = std::sin(yaw * h), cy = std::cos(yaw * h);
        return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
    }

    inline double normalize_axis(double angle)
    {
        angle = std::fmod(angle, 360.0);
        if (angle > 180.0) angle -= 360.0;
        if (angle < -180.0) angle += 360.0;
        return angle;
    }

    // UE FQuat::Rotator, degrees out.
    inline void to_rotator(Quat q, double& pitch, double& yaw, double& roll)
    {
        const double rad_to_deg = 180.0 / PI;
        const double test = q.z * q.x - q.w * q.y;
        const double yaw_y = 2.0 * (q.w * q.z + q.x * q.y);
        const double yaw_x = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
        const double threshold = 0.4999995;
        yaw = std::atan2(yaw_y, yaw_x) * rad_to_deg;
        if (test < -threshold)
        {
            pitch = -90.0;
            roll = normalize_axis(-yaw - 2.0 * std::atan2(q.x, q.w) * rad_to_deg);
        }
        else if (test > threshold)
        {
            pitch = 90.0;
            roll = normalize_axis(yaw - 2.0 * std::atan2(q.x, q.w) * rad_to_deg);
        }
        else
        {
            pitch = std::asin(2.0 * test) * rad_to_deg;
            roll = std::atan2(-2.0 * (q.w * q.x + q.y * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)) * rad_to_deg;
        }
    }

    inline Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

    inline Quat multiply(Quat a, Quat b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    inline Vec3 rotate(Quat q, Vec3 v)
    {
        Quat p{v.x, v.y, v.z, 0.0};
        Quat r = multiply(multiply(q, p), conjugate(q));
        return {r.x, r.y, r.z};
    }

    inline Quat normalize(Quat q)
    {
        double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n < 1e-12) return {};
        return {q.x / n, q.y / n, q.z / n, q.w / n};
    }

    inline Quat slerp(Quat a, Quat b, double t)
    {
        double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (dot < 0.0)
        {
            b = {-b.x, -b.y, -b.z, -b.w};
            dot = -dot;
        }
        if (dot > 0.9995)
        {
            return normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        }
        double theta = std::acos(std::clamp(dot, -1.0, 1.0));
        double s = std::sin(theta);
        double wa = std::sin((1.0 - t) * theta) / s;
        double wb = std::sin(t * theta) / s;
        return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    }

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

    // Frame-rate independent: the same lag closes at the same speed at any frame time.
    inline double follow_alpha(double rate, int kind, double lag, double catchup, double floor_scale, double dt)
    {
        double scale = kind == 0 ? 1.0 : floor_scale + (1.0 - floor_scale) * curve(kind, catchup > 0.0 ? lag / catchup : 1.0);
        return 1.0 - std::exp(-std::max(rate, 0.0) * scale * dt);
    }
} // namespace dwsc
