// UE4SS's log, as a sink: the harness compares state, not log lines.
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
