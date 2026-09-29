// The one verbose-log switch (smoothwalker.ini log_verbose): a Verbose line is sent only while it is set. Written by
// Smoothwalker's settings (the early read at construction, every publish) and by CameraCore::set_diagnostics
// (DIAG_VERBOSE, from the same publish); read by the camera core's and Smoothwalker's verbose lines on any thread.
// Image-level and never reset: each instance's construction writes it before its first verbose line.
#pragma once

#include <atomic>

namespace dw
{
    inline std::atomic<bool> g_verbose{false};

    inline auto verbose() -> bool
    {
        return g_verbose.load(std::memory_order_relaxed);
    }
} // namespace dw
