// The GetCameraView hook on vtable slot 214 and the image-level statics (docs/design.md, "The DLL is pinned"): the
// hook's relation to the vtable, which outlives any one instance of the mod on the pinned image, and the slot through
// which the hook reaches the live instance's Pipeline. Everything else of the hook side is the Pipeline's
// (camera/pipeline.hpp).
#pragma once

#include "pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace dw::camera
{
    constexpr size_t GET_CAMERA_VIEW_SLOT = 214;

    using GetCameraViewFn = void(__fastcall*)(void* self, float delta_time, void* desired_view);

    // The hook itself: the game's GetCameraView, then the published Pipeline's part of the update, if one is.
    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view);

    // Counts one construction of the core in this process; true when the image had started before (a hot reload).
    auto note_start() -> bool;
    // The Pipeline the hook calls from now on (the Core's, from its constructor). It stays alive until
    // unpublish_pipeline() returns true.
    auto publish_pipeline(Pipeline& pipeline) -> void;
    // Hooks the slot at `entry` (the RebelCameraComponent vtable's slot 214), or keeps the hook a previous instance
    // left there. Logs the outcome; false: not hooked, the mod is inactive.
    auto hook_slot(uintptr_t** entry) -> bool;
    // Puts back what hook_slot replaced, if the slot still holds this hook, then sleeps 50 ms. Logs a slot left alone.
    auto restore_slot() -> void;
    // No hook call reaches the Pipeline any more; waits up to 5 s for the calls in flight. False: one is still running
    // (logged), and the Pipeline must stay allocated.
    auto unpublish_pipeline() -> bool;
} // namespace dw::camera
