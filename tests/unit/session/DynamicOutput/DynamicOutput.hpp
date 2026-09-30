// UE4SS's log as a sink, for camera/hook.cpp in dw_unit (tests/unit/CMakeLists.txt puts this folder on hook.cpp's
// include path only): the session suite checks state, not log lines.
#pragma once

#define STR(text) L##text

namespace RC
{
    enum class LogLevel
    {
        Default,
        Normal,
        Verbose,
        Warning,
        Error,
    };

    struct Output
    {
        template <LogLevel Level, typename... Args>
        static auto send(Args&&...) -> void
        {
        }
    };
} // namespace RC
