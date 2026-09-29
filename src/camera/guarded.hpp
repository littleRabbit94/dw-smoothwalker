// A memory copy that answers false instead of faulting, for the hook's reads of the player's objects and its write of
// the view. Injected into the Pipeline (seh_copy by default), so a test fails chosen accesses.
#pragma once

#include <cstddef>

namespace dw::camera
{
    // Copies n bytes from src to dst; false, with dst in any state, when an access faulted.
    using GuardedCopy = bool (*)(void* dst, const void* src, size_t n);

    // Structured exception handling around memcpy (camera/guarded.cpp).
    auto seh_copy(void* dst, const void* src, size_t n) -> bool;
} // namespace dw::camera
