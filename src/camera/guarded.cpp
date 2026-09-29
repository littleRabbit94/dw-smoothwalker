// seh_copy (camera/guarded.hpp): the only __try in the camera code, alone in its translation unit. Its frame holds no
// C++ object with a destructor, which __try requires.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "guarded.hpp"

#include <cstring>

namespace dw::camera
{
    auto seh_copy(void* dst, const void* src, size_t n) -> bool
    {
        __try
        {
            memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
} // namespace dw::camera
