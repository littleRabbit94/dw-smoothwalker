// The Windows surface the hook region, config.hpp, lua_api.hpp and the camera's .cpp files use, for a Linux build of
// the equivalence harness. The clock is the harness's (QueryPerformanceCounter advances by one tick per call, so the
// two variants must make the same calls in the same order to stay in step); locks are no-ops
// (one thread); the file and string calls fail harmlessly (never reached).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harness
{
    auto qpc_tick() -> int64_t;  // the clock now, then one tick on
    auto fault_now() -> bool;    // true when the harness armed a fault for this guarded memory access
} // namespace harness

using BOOL = int;
using DWORD = unsigned long;
using UINT = unsigned int;
using LONG = long;
using HANDLE = void*;
using HMODULE = void*;
using LPCWSTR = const wchar_t*;
using LPCSTR = const char*;
using WCHAR = wchar_t;

#define TRUE 1
#define FALSE 0

union LARGE_INTEGER
{
    struct
    {
        uint32_t LowPart;
        int32_t HighPart;
    } u;
    int64_t QuadPart;
};

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* out)
{
    out->QuadPart = harness::qpc_tick();
    return TRUE;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* out)
{
    out->QuadPart = 10'000'000;
    return TRUE;
}

struct SRWLOCK
{
    void* Ptr;
};
#define SRWLOCK_INIT {nullptr}
inline void AcquireSRWLockShared(SRWLOCK*) {}
inline void ReleaseSRWLockShared(SRWLOCK*) {}
inline void AcquireSRWLockExclusive(SRWLOCK*) {}
inline void ReleaseSRWLockExclusive(SRWLOCK*) {}

inline void Sleep(DWORD) {}

// The slot 214 install (camera/hook.cpp): a harness-owned slot, always writable; nothing is ever mapped.
#define PAGE_READWRITE 0x04
inline BOOL VirtualProtect(void*, size_t, DWORD protect, DWORD* old)
{
    if (old) *old = protect;
    return TRUE;
}
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4
inline BOOL GetModuleHandleExW(DWORD, LPCWSTR, HMODULE* out)
{
    if (out) *out = nullptr;
    return FALSE;
}
inline DWORD GetCurrentThreadId() { return 1; }
inline DWORD GetLastError() { return 0; }

// Structured exception handling is in seh.hpp: libstdc++ owns the name __try.
#define EXCEPTION_EXECUTE_HANDLER 1
#define __fastcall

// Files and strings (config.hpp): never reached by the harness, only compiled.
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_WRITE_THROUGH 0x8
inline BOOL MoveFileExA(LPCSTR, LPCSTR, DWORD) { return FALSE; }
inline BOOL DeleteFileA(LPCSTR) { return FALSE; }
inline BOOL CreateDirectoryA(LPCSTR, void*) { return FALSE; }
inline BOOL CreateDirectoryW(LPCWSTR, void*) { return FALSE; }

#define CP_UTF8 65001
#define WC_ERR_INVALID_CHARS 0x80
#define MB_ERR_INVALID_CHARS 0x8
inline int WideCharToMultiByte(UINT, DWORD, const wchar_t*, int, char*, int, const char*, BOOL*) { return 0; }
inline int MultiByteToWideChar(UINT, DWORD, const char*, int, wchar_t*, int) { return 0; }

#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
struct WIN32_FIND_DATAW
{
    DWORD dwFileAttributes;
    wchar_t cFileName[260];
};
inline HANDLE FindFirstFileW(LPCWSTR, WIN32_FIND_DATAW*) { return INVALID_HANDLE_VALUE; }
inline BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW*) { return FALSE; }
inline BOOL FindClose(HANDLE) { return TRUE; }

#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
inline int CompareStringOrdinal(const wchar_t*, int, const wchar_t*, int, BOOL) { return CSTR_GREATER_THAN; }
