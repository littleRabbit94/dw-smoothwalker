// The clock the camera core reads: QueryPerformanceCounter ticks and their frequency. Injected (the Pipeline and the
// Authority each take one, QPC_CLOCK by default), so a test drives time itself. Every tick the core reads goes
// through a Clock's now(), in a fixed order per call path.
// Needs the Windows types included before it.
#pragma once

#include <cstdint>

namespace dw::camera
{
    struct Clock
    {
        int64_t (*now)();       // ticks now
        double (*frequency)();  // ticks per second, constant for the process
    };

    inline auto qpc_now() -> int64_t
    {
        LARGE_INTEGER li{};
        QueryPerformanceCounter(&li);
        return li.QuadPart;
    }

    // Constant per boot: a function-local static, never reset.
    inline auto qpc_frequency() -> double
    {
        static const double f = [] {
            LARGE_INTEGER li{};
            QueryPerformanceFrequency(&li);
            return static_cast<double>(li.QuadPart);
        }();
        return f;
    }

    constexpr Clock QPC_CLOCK{&qpc_now, &qpc_frequency};
} // namespace dw::camera
