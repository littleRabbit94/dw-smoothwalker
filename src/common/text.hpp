// UTF-8 and UTF-16 conversions for file names, labels, banners and log lines (Win32 MultiByteToWideChar and
// WideCharToMultiByte). Header-only, no state.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <optional>
#include <string>

namespace dw
{
    inline auto utf8_of(const std::wstring& w) -> std::optional<std::string>
    {
        if (w.empty()) return std::string{};
        int size = static_cast<int>(w.size());
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), size, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::nullopt;
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), size, out.data(), n, nullptr, nullptr);
        return out;
    }

    // Strict: nullopt for invalid UTF-8, which would make the Mod Menu skip the whole manifest.
    inline auto wide_of_utf8(const std::string& s) -> std::optional<std::wstring>
    {
        if (s.empty()) return std::wstring{};
        int size = static_cast<int>(s.size());
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), size, nullptr, 0);
        if (n <= 0) return std::nullopt;
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), size, out.data(), n);
        return out;
    }

    // For log and banner text: invalid bytes become U+FFFD.
    inline auto to_wide(const std::string& s) -> std::wstring
    {
        if (s.empty()) return {};
        int size = static_cast<int>(s.size());
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), size, nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), size, out.data(), n);
        return out;
    }
} // namespace dw
