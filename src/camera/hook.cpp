// The GetCameraView hook on vtable slot 214 and the statics that must outlive one instance of the mod (camera/hook.hpp).
// The image is pinned (camera/core.cpp, pin_module), so these survive a hot reload with their values, and no static
// initializer runs again. Kept on purpose, never reset: g_original, g_vtable_entry and g_hook_left (hook_slot reads
// them on the reused image), g_in_hook (self-balancing; a call through a hook left in the chain may be in flight),
// g_starts (one count per process). g_pipeline is the one exception: per-instance state, reset by
// Core::reset_globals until the core owns it.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "hook.hpp"

#include <atomic>

#include <DynamicOutput/DynamicOutput.hpp>

namespace dw::camera
{
namespace
{
    GetCameraViewFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;
    bool g_hook_left = false;      // the last unload found another mod's hook over ours and left the slot alone
    std::atomic<int> g_in_hook{0}; // calls running inside get_camera_view_hook; unload waits for 0
    int g_starts = 0;              // constructions in this process: one image, pinned (pin_module)

    struct InHook
    {
        InHook() { g_in_hook.fetch_add(1, std::memory_order_relaxed); }
        ~InHook() { g_in_hook.fetch_sub(1, std::memory_order_relaxed); }
    };
} // namespace

    Pipeline g_pipeline;

    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view)
    {
        InHook in_hook;
        g_original(self, delta_time, desired_view);
        g_pipeline.on_camera_view(self, delta_time, desired_view);
    }

    auto note_start() -> bool
    {
        return g_starts++ > 0;
    }

    auto hook_slot(uintptr_t** entry) -> bool
    {
        auto* hook = reinterpret_cast<uintptr_t*>(&get_camera_view_hook);
        // A hot reload restarts on the same pinned image, and the last unload may have left the hook in place: it
        // found another mod's hook over ours (g_hook_left), or that mod has since unhooked and put ours back.
        // g_original then still holds what we called before. Capturing the slot again while our hook is reached
        // would store the hook itself, or the other mod's hook that calls it, as the original: endless recursion on
        // the first camera update. So the previous original is kept, except where the slot provably bypasses us:
        // it holds g_original itself (a mod hooked below us wrote it back at its unload, then hooked again at the
        // same address, or never hooked again), and nothing reached from the slot is known to call our hook, so a
        // fresh capture is taken. A mod below us that captured our hook as its own original (one above us put ours
        // back first) still loops; so did the code before the pin.
        const bool bypassed = g_hook_left && *entry != hook && reinterpret_cast<GetCameraViewFn>(*entry) == g_original;
        if (bypassed) g_hook_left = false;
        if (!bypassed && (*entry == hook || g_hook_left))
        {
            // Unmapped: the mod we called was freed. Keeping it crashes, recapturing may recurse; neither is safe.
            // Refusing only stops the log from claiming success: a hook still in the chain calls the freed original
            // anyway. A trampoline outside any module (a hooking library's VirtualAlloc) also reads as unmapped.
            HMODULE owner{};
            bool mapped = g_original && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                           reinterpret_cast<LPCWSTR>(g_original), &owner);
            if (!mapped || g_original == &get_camera_view_hook || g_vtable_entry != entry)
            {
                RC::Output::send<RC::LogLevel::Error>(STR("[DWSmoothwalker] slot {} was left hooked by the last unload and no usable original is known; "
                                                          "smoothing off, restart the game\n"),
                                                      GET_CAMERA_VIEW_SLOT);
                return false;
            }
            if (*entry == hook)
            {
                RC::Output::send<RC::LogLevel::Normal>(STR("[DWSmoothwalker] GetCameraView hook kept from before the reload (slot {})\n"), GET_CAMERA_VIEW_SLOT);
            }
            else
            {
                // Another pointer: whether it still calls ours cannot be seen from here.
                RC::Output::send<RC::LogLevel::Warning>(STR("[DWSmoothwalker] slot {} holds another mod's hook: assuming it still calls this mod's hook from "
                                                            "before the reload. If smoothing does not work, restart the game to hook cleanly\n"),
                                                        GET_CAMERA_VIEW_SLOT);
            }
            return true;
        }
        DWORD prev{};
        if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &prev))
        {
            RC::Output::send<RC::LogLevel::Error>(STR("[DWSmoothwalker] vtable protect failed, mod inactive\n"));
            return false;
        }
        g_original = reinterpret_cast<GetCameraViewFn>(*entry);
        *entry = hook;
        VirtualProtect(entry, sizeof(*entry), prev, &prev);
        g_vtable_entry = entry;
        RC::Output::send<RC::LogLevel::Normal>(STR("[DWSmoothwalker] GetCameraView hooked (slot {})\n"), GET_CAMERA_VIEW_SLOT);
        return true;
    }

    // UE4SS FreeLibrary's the DLL right after the mod's destructor (hot reload); pinned, the image stays mapped. A
    // call already in the hook must return before the core's state is reset.
    auto restore_slot() -> void
    {
        g_hook_left = true; // until the restore below succeeds
        DWORD prev{};
        if (VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), PAGE_READWRITE, &prev))
        {
            // Only this mod's own entry is put back: another mod hooked after us would otherwise be unhooked too.
            // Left in place, the hook stays callable through that mod (the image is pinned) and the next start
            // keeps g_original (hook_slot).
            if (*g_vtable_entry == reinterpret_cast<uintptr_t*>(&get_camera_view_hook))
            {
                *g_vtable_entry = reinterpret_cast<uintptr_t*>(g_original);
                g_hook_left = false;
            }
            else
            {
                RC::Output::send<RC::LogLevel::Warning>(STR("[DWSmoothwalker] unload: slot {} no longer holds this mod's hook, left as is\n"), GET_CAMERA_VIEW_SLOT);
            }
            VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), prev, &prev);
        }
        // A worker may have read the old entry just before the restore and not entered the hook yet.
        Sleep(50);
        for (int i = 0; i < 500 && g_in_hook.load() != 0; ++i) Sleep(10);
        if (g_in_hook.load() != 0) RC::Output::send<RC::LogLevel::Warning>(STR("[DWSmoothwalker] unload: a camera update is still in the hook\n"));
    }
} // namespace dw::camera
