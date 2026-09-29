// The GetCameraView hook on vtable slot 214 and the image-level statics (docs/design.md, "The DLL is pinned"): the
// hook's relation to the vtable, which outlives any one instance of the mod on the pinned image, and, for now, the one
// Pipeline the hook reaches. Everything else of the hook side is the Pipeline's (camera/pipeline.hpp).
#pragma once

#include "pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace dw::camera
{
    constexpr size_t GET_CAMERA_VIEW_SLOT = 214;

    using GetCameraViewFn = void(__fastcall*)(void* self, float delta_time, void* desired_view);

    // The hook itself: the game's GetCameraView, then the Pipeline's part of the update.
    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view);

    // The one Pipeline, image-level for now: reset by Core::reset_globals, since a hot reload runs no static
    // initializer on the pinned image.
    extern Pipeline g_pipeline;

    // Counts one construction of the core in this process; true when the image had started before (a hot reload).
    auto note_start() -> bool;
    // Hooks the slot at `entry` (the RebelCameraComponent vtable's slot 214), or keeps the hook a previous instance
    // left there. Logs the outcome; false: not hooked, the mod is inactive.
    auto hook_slot(uintptr_t** entry) -> bool;
    // Puts back what hook_slot replaced, if the slot still holds this hook, then waits up to ~5 s for calls still
    // inside the hook. Logs a slot left alone and a call still running.
    auto restore_slot() -> void;
} // namespace dw::camera
