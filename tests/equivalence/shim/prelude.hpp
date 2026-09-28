// Included by both drivers before any product header. Every standard header the product headers use goes in
// first, so none of them is read after the two macros below:
// - SEH: `__try { ... } __except (...) { ... }` becomes `if (!fault) { ... } else { ... }`, a fault the harness can
//   arm for one guarded memory access. libstdc++ defines __try itself (bits/exception_defines.h), hence the order.
// - `std::ifstream(std::wstring, mode)` is an MSVC extension config.hpp uses for preset files; here it opens nothing.
#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#undef __try
#define __try if (!::harness::fault_now())
#define __except(filter) else

namespace std
{
    struct harness_ifstream : ifstream
    {
        using ifstream::ifstream;
        harness_ifstream(const wstring&, ios_base::openmode) {} // never opened
    };
} // namespace std
#define ifstream harness_ifstream
