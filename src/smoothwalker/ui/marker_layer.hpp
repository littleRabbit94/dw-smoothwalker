// Debug markers' widget (debug_markers, debug_key): a full-screen UMG canvas of Borders and one TextBlock that draws
// the camera follow's trail on the ground around the player, updated every engine tick. The numbers are
// ui/markers.hpp's (projection, pivots, trail, layout); this side builds the widgets once and moves them. Built,
// moved and removed on the game thread only, from the engine tick; the GetCameraView hook never touches it.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "markers.hpp"
#include "../../common/live_ref.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace dw::smoothwalker::ui
{
    using namespace RC::Unreal;

    // The layer: UserWidget -> CanvasPanel (the root, filling the viewport) -> markers::COUNT Borders and a TextBlock,
    // each in a CanvasPanelSlot centred on its position, hit-test invisible, Z order 999 (under the debug overlay's
    // 1000). Proven live (2026-09-30): a canvas constructed with the widget tree as outer and set as RootWidget,
    // children added with AddChildToCanvas, and the Borders' brush written before AddToViewport. All styling is
    // written then; per tick only what changed since the last tick is sent (position, size, angle, opacity,
    // visibility, the label's text). Nothing here runs from the destructor: see forget().
    class MarkerLayer
    {
      public:
        // Once per engine tick. on: the debug_markers setting. controller, camera: the player's, checked live this
        // tick, or nullptr. gather() returns the frame's markers::Scene; it runs only while the layer is built.
        // Built only with a player controller and camera, so the main menu never shows it; without them it hides.
        template <typename Gather>
        auto tick(bool on, UObject* controller, UObject* camera, Gather&& gather) -> void
        {
            if (!on)
            {
                hide();
                return;
            }
            if (!controller || !camera)
            {
                collapse(); // the main menu, a load, or between pawns
                return;
            }
            if (m_state == 0) resolve();
            if (m_state != 1) return;
            auto now = std::chrono::steady_clock::now();
            if (m_widget.object && !alive()) forget();
            if (!m_widget.object)
            {
                if (now < m_next_build) return;
                m_next_build = now + RETRY;
                if (!build(controller)) return;
                m_next_check = now + CHECK;
            }
            else if (now >= m_next_check)
            {
                m_next_check = now + CHECK;
                if (!in_viewport())
                {
                    forget(); // something else took it off the screen: build anew at the build cadence
                    m_next_build = now + RETRY;
                    return;
                }
            }
            draw(controller, gather());
        }

        // A level change: the viewport has dropped the layer, so the next tick builds a new one. Only the references,
        // the send caches and the markers state go: nothing is called on the old widgets. A hot reload calls nothing
        // here at all (the destructor runs off the game thread), so the last frame stays on screen, frozen, until the
        // next level change removes it.
        auto forget() -> void;

      private:
        static constexpr auto RETRY = std::chrono::seconds(2);        // a failed build is not retried every tick
        static constexpr auto CHECK = std::chrono::milliseconds(250); // how often IsInViewport is asked

        // One widget and its slot, with what was last sent to them. NAN: never sent.
        struct Shape
        {
            LiveRef widget, slot;
            bool shown = false;
            double x = NAN, y = NAN, w = NAN, h = NAN, angle = 0.0, opacity = 1.0;
        };

        int m_state = 0; // 0 unresolved, 1 ready, -1 unavailable
        LiveRef m_widget, m_canvas;
        std::array<Shape, markers::COUNT> m_shapes{};
        Shape m_label;
        int m_label_cm = -1; // the label's text as last set; -1 none
        bool m_root_shown = false;
        markers::State m_markers; // the trail and the lift, cleared whenever the layer hides
        std::chrono::steady_clock::time_point m_next_build{}, m_next_check{};
        bool m_build_warned = false;

        UObject* m_library = nullptr;        // WidgetBlueprintLibrary CDO
        UObject* m_layout_library = nullptr; // WidgetLayoutLibrary CDO
        UFunction* m_create = nullptr;
        UFunction* m_add = nullptr;
        UFunction* m_remove = nullptr;
        UFunction* m_is_in_viewport = nullptr; // optional: without it a layer removed by the game waits for its GC
        UFunction* m_add_child = nullptr;      // CanvasPanel:AddChildToCanvas
        UFunction* m_slot_position = nullptr;  // CanvasPanelSlot:SetPosition, SetSize, SetAlignment, SetAutoSize
        UFunction* m_slot_size = nullptr;
        UFunction* m_slot_alignment = nullptr;
        UFunction* m_slot_auto_size = nullptr;
        UFunction* m_set_angle = nullptr;      // Widget:SetRenderTransformAngle, SetRenderOpacity, SetVisibility
        UFunction* m_set_opacity = nullptr;
        UFunction* m_set_visibility = nullptr;
        UFunction* m_set_text = nullptr;       // TextBlock:SetText
        UFunction* m_viewport_size = nullptr;  // WidgetLayoutLibrary:GetViewportSize, GetViewportScale
        UFunction* m_viewport_scale = nullptr;
        UClass* m_user_widget = nullptr;
        UClass* m_canvas_panel = nullptr;
        UClass* m_border = nullptr;
        UClass* m_text_block = nullptr;

        int32_t m_create_world = -1, m_create_type = -1, m_create_player = -1, m_create_return = -1;
        int32_t m_widget_tree = -1, m_root_widget = -1; // UserWidget::WidgetTree, WidgetTree::RootWidget
        int32_t m_z_order = -1, m_in_viewport_return = -1;
        int32_t m_child_content = -1, m_child_return = -1;
        int32_t m_size_world = -1, m_scale_world = -1;
        FProperty* m_position = nullptr;  // SetPosition's InPosition (FVector2D)
        FProperty* m_size = nullptr;      // SetSize's InSize
        FProperty* m_alignment = nullptr; // SetAlignment's InAlignment
        FProperty* m_auto_size = nullptr; // SetAutoSize's InbAutoSize
        FProperty* m_angle = nullptr;     // SetRenderTransformAngle's Angle
        FProperty* m_opacity = nullptr;   // SetRenderOpacity's InOpacity
        int32_t m_visibility_param = -1;  // SetVisibility's InVisibility (one byte)
        int32_t m_text_param = -1;        // SetText's InText
        FProperty* m_size_return = nullptr;  // GetViewportSize's ReturnValue (FVector2D)
        FProperty* m_scale_return = nullptr; // GetViewportScale's ReturnValue
        // The vectors' X and Y, found once in resolve: the per-tick sends and reads never look a member up by name.
        FProperty* m_position_xy[2]{};
        FProperty* m_size_xy[2]{};
        FProperty* m_alignment_xy[2]{};
        FProperty* m_size_return_xy[2]{};
        FProperty* m_visibility = nullptr;   // Widget::Visibility, written before AddToViewport
        // Styling, optional: a missing one leaves the engine default.
        FProperty* m_brush_color = nullptr;  // Border::BrushColor (FLinearColor)
        FProperty* m_background = nullptr;   // Border::Background (FSlateBrush)
        FProperty* m_draw_as = nullptr;      // its DrawAs (one byte): RoundedBox for dots and rings
        FProperty* m_outline = nullptr;      // its OutlineSettings (FSlateBrushOutlineSettings)
        FProperty* m_rounding = nullptr;     // OutlineSettings.RoundingType (one byte): HalfHeightRadius, a circle
        FProperty* m_outline_width = nullptr;
        FProperty* m_outline_color = nullptr;        // OutlineSettings.Color (FSlateColor, SpecifiedColor)
        FProperty* m_outline_transparency = nullptr; // OutlineSettings.bUseBrushTransparency
        FProperty* m_font = nullptr;         // TextBlock::Font (FSlateFontInfo, Size)
        FProperty* m_text_color = nullptr;  // TextBlock::ColorAndOpacity (FSlateColor, SpecifiedColor)

        // The projection settings (markers::Viewport) and the rendered view (markers::Scene::camera), read from the
        // player's camera manager and local player each drawn tick. Settings missing: the measured defaults; the view
        // missing: the feed's shown view. One warning each.
        bool m_projection_ok = false;
        bool m_pov_view_ok = false;
        UClass* m_camera_manager_class = nullptr;
        UClass* m_local_player_class = nullptr;
        int32_t m_camera_manager_at = -1; // PlayerController::PlayerCameraManager
        int32_t m_player_at = -1;         // PlayerController::Player
        int32_t m_pov_at = -1;            // PlayerCameraManager::CameraCachePrivate.POV, from the manager
        FProperty* m_aspect_ratio = nullptr; // FMinimalViewInfo::AspectRatio
        FProperty* m_constrain = nullptr;    // FMinimalViewInfo::bConstrainAspectRatio (a bitfield)
        FProperty* m_pov_location = nullptr; // FMinimalViewInfo::Location (FVector: X, Y, Z)
        FProperty* m_pov_rotation = nullptr; // FMinimalViewInfo::Rotation (FRotator: Pitch, Yaw, Roll)
        FProperty* m_pov_fov = nullptr;      // FMinimalViewInfo::FOV
        FProperty* m_pov_location_xyz[3]{};  // Location's X, Y, Z and Rotation's Pitch, Yaw, Roll, found once
        FProperty* m_pov_rotation_pyr[3]{};
        int32_t m_axis_at = -1;              // LocalPlayer::AspectRatioAxisConstraint (one byte)

        // Once, the first time the layer is wanted. Everything that builds, places or feeds the layer is required; the
        // styling and the projection settings are not.
        auto resolve() -> void;
        auto resolve_projection() -> void;
        auto build(UObject* controller) -> bool;
        // False when the widget, the canvas or any child or slot has gone (a GC).
        auto alive() const -> bool;
        // True unless Widget:IsInViewport says the layer is off the screen (true when that function is missing).
        auto in_viewport() -> bool;
        // The viewport's size and scale and the projection settings; the rendered view into `scene`.
        auto viewport(UObject* controller, markers::Scene& scene) -> markers::Viewport;
        auto draw(UObject* controller, markers::Scene scene) -> void;
        auto apply(Shape& shape, const markers::Primitive& p) -> void;
        auto apply_label(const markers::Label& label) -> void;
        auto set_root_shown(bool shown) -> void;
        // Writes a new Border's brush for its look and colour (markers::look, markers::color), before AddToViewport.
        auto style(UObject* border, int index) -> void;
        auto set_visibility(UObject* widget, uint8_t visibility) -> void;
        auto set_xy(UObject* slot, UFunction* function, FProperty* parameter, FProperty* const (&xy)[2], double x, double y) -> void;
        auto set_real(UObject* widget, UFunction* function, FProperty* parameter, double value) -> void;
        // Hidden: collapsed in place, the markers state cleared. The widget stays built.
        auto collapse() -> void;
        // The layer leaves the viewport and is collected; switched on again, a new one is built.
        auto hide() -> void;
    };
} // namespace dw::smoothwalker::ui
