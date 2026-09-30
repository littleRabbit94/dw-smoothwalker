// Debug overlay (debug_overlay, debug_key): a UMG text panel at the top right of the screen with the live follow,
// the player's camera modes, the last hard cut and the camera API claim (docs/design.md, "Debug overlay").
// Built, refreshed and removed on the game thread only, from the engine tick. The GetCameraView hook never touches
// it: the core publishes numbers that the refresh reads through its interface (CameraCore::read_debug, camera_owner;
// smoothwalker/smoothwalker.cpp, debug_panel), and the text is laid out by ui/panel.hpp.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "../../common/live_ref.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace dw::smoothwalker::ui
{
    using namespace RC::Unreal;

    // The panel: UserWidget -> Border (translucent black, padded) -> TextBlock (Roboto, off-white), anchored top
    // right, hit-test invisible. Game thread only. Proven live by a Lua probe (2026-09-23): WidgetBlueprintLibrary
    // Create with a plain UserWidget gives a widget whose WidgetTree already exists, and a widget constructed with
    // that tree as its outer and set as RootWidget before AddToViewport paints. Styling is written into reflected
    // properties before AddToViewport, where the Slate widgets are built and SynchronizeProperties applies them.
    // Structs passed to a UFunction are written through their members' FProperty offsets, never a hardcoded
    // layout (UE 5's FVector2D is two doubles). Nothing here runs from the destructor: see forget().
    class DebugOverlay
    {
      public:
        // Once per engine tick. on: the debug_overlay setting. controller, camera: the player's, checked live this tick,
        // or nullptr. compose() returns the panel's text; it runs at most every REFRESH, only while the panel is up.
        // Built only with a player camera, so the main menu never shows it; rebuilt after a GC or a level change.
        template <typename Compose>
        auto tick(bool on, UObject* controller, UObject* camera, Compose&& compose) -> void
        {
            if (!on)
            {
                hide();
                return;
            }
            if (!controller || !camera) return; // the main menu, a load, or between pawns: a panel already up stays
            if (m_state == 0) resolve();
            if (m_state != 1) return;
            auto now = std::chrono::steady_clock::now();
            if (m_widget.object && !(m_widget.alive() && m_text.alive())) forget();
            if (!m_widget.object)
            {
                if (now < m_next_build) return;
                m_next_build = now + RETRY;
                if (build(controller, compose())) m_next_refresh = now + REFRESH;
                return;
            }
            if (now < m_next_refresh) return;
            m_next_refresh = now + REFRESH;
            if (!in_viewport())
            {
                forget(); // something else took it off the screen (a widget clear without a level change): build anew
                m_next_build = now + RETRY; // at the build cadence, not on the next tick
                return;
            }
            auto text = compose();
            if (text != m_shown) set_text(std::move(text));
        }

        // A level change: the viewport has dropped the panel, so the next tick builds a new one. Only the references
        // go: nothing is called on the old widget. A hot reload calls nothing here at all (the destructor runs off the
        // game thread), so the last text stays on screen, frozen, until the next level change removes it.
        auto forget() -> void
        {
            m_widget = m_text = {};
            m_shown.clear();
        }

      private:
        static constexpr auto REFRESH = std::chrono::milliseconds(250);
        static constexpr auto RETRY = std::chrono::seconds(2); // a failed build is not retried every tick

        int m_state = 0; // 0 unresolved, 1 ready, -1 unavailable
        LiveRef m_widget, m_text;
        std::wstring m_shown; // text last set
        std::chrono::steady_clock::time_point m_next_build{}, m_next_refresh{};
        bool m_build_warned = false;

        UObject* m_library = nullptr; // WidgetBlueprintLibrary CDO
        UFunction* m_create = nullptr;
        UFunction* m_set_content = nullptr;
        UFunction* m_set_text = nullptr;
        UFunction* m_add = nullptr;
        UFunction* m_remove = nullptr;
        UFunction* m_set_position = nullptr;
        UFunction* m_set_anchors = nullptr;
        UFunction* m_set_alignment = nullptr;
        UFunction* m_is_in_viewport = nullptr; // optional: without it a panel removed by the game waits for its GC
        int32_t m_in_viewport_return = -1;
        UClass* m_user_widget = nullptr;
        UClass* m_border = nullptr;
        UClass* m_text_block = nullptr;

        int32_t m_create_world = -1, m_create_type = -1, m_create_player = -1, m_create_return = -1;
        int32_t m_widget_tree = -1, m_root_widget = -1; // UserWidget::WidgetTree, WidgetTree::RootWidget
        int32_t m_content = -1, m_text_param = -1, m_z_order = -1;
        FProperty* m_position = nullptr;  // SetPositionInViewport's Position (FVector2D)
        FProperty* m_anchors = nullptr;   // SetAnchorsInViewport's Anchors (FAnchors: Minimum, Maximum)
        FProperty* m_alignment = nullptr; // SetAlignmentInViewport's Alignment
        FProperty* m_visibility = nullptr;
        // Styling, optional: a missing one leaves the engine default.
        FProperty* m_brush_color = nullptr; // Border::BrushColor (FLinearColor)
        FProperty* m_padding = nullptr;     // Border::Padding (FMargin)
        FProperty* m_font = nullptr;        // TextBlock::Font (FSlateFontInfo, Size)
        FProperty* m_text_color = nullptr;  // TextBlock::ColorAndOpacity (FSlateColor, SpecifiedColor)

        // Once, the first time the panel is wanted. Everything that places or feeds the panel is required; the
        // styling is not.
        auto resolve() -> void;
        auto build(UObject* controller, std::wstring text) -> bool;
        auto set_text(std::wstring value) -> void;
        // True unless Widget:IsInViewport says the panel is off the screen (true when that function is missing).
        auto in_viewport() -> bool;
        // The widget leaves the viewport and is collected; switched on again, a new one is built.
        auto hide() -> void;
    };
} // namespace dw::smoothwalker::ui
