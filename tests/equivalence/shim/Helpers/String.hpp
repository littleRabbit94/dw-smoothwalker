// UE4SS's string helper lua_api.hpp names.
#pragma once

#include <string>

namespace RC
{
    inline auto to_wstring(const std::string& s) -> std::wstring
    {
        return std::wstring(s.begin(), s.end());
    }
} // namespace RC
