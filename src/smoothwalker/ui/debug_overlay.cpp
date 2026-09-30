// The debug overlay's widget (ui/debug_overlay.hpp): reflection lookups, the UMG panel's construction and its
// text updates. Bodies moved from debug_overlay.hpp unchanged.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "debug_overlay.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <string>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dw::smoothwalker::ui
{
    using namespace RC;
    using namespace RC::Unreal;

namespace
{
    constexpr size_t PARAMS = 256; // every params buffer here; checked against reflection
    constexpr int32_t Z_ORDER = 1000;
    constexpr double INSET = 16.0;            // slate units from the right and top edges
    constexpr uint8_t HIT_TEST_INVISIBLE = 3; // ESlateVisibility: drawn, never takes input
    constexpr double FONT_SIZE = 22.0;
    constexpr double PADDING = 8.0;
    constexpr double BACKGROUND[4]{0.0, 0.0, 0.0, 0.6};
    constexpr double TEXT_COLOR[4]{0.92, 0.92, 0.88, 1.0};

    auto find_property(UStruct* owner, const wchar_t* name) -> FProperty*
    {
        if (!owner) return nullptr;
        for (FProperty* property : TFieldRange<FProperty>(owner))
        {
            // FNames compare without case, and the stored spelling is the first one registered: the game
            // has SetPositionInViewport's parameter as "position".
            if (_wcsicmp(property->GetName().c_str(), name) == 0) return property;
        }
        return nullptr;
    }

    // A member of a struct-typed property.
    auto member(FProperty* of, const wchar_t* name) -> FProperty*
    {
        auto* as_struct = CastField<FStructProperty>(of);
        return as_struct ? find_property(as_struct->GetStruct().Get(), name) : nullptr;
    }

    // The offset of a pointer-sized parameter or property, or -1.
    auto pointer_at(UStruct* owner, const wchar_t* name) -> int32_t
    {
        auto* property = find_property(owner, name);
        return property && property->GetElementSize() == static_cast<int32_t>(sizeof(void*)) ? property->GetOffset_ForInternal() : -1;
    }

    auto value_in(FProperty* property, void* container) -> uint8_t*
    {
        return static_cast<uint8_t*>(container) + property->GetOffset_ForInternal();
    }

    // A float or a double property; false for anything else, which is left alone.
    auto write_real(FProperty* property, void* container, double value) -> bool
    {
        if (!property) return false;
        if (CastField<FDoubleProperty>(property))
        {
            memcpy(value_in(property, container), &value, sizeof(value));
            return true;
        }
        if (CastField<FFloatProperty>(property))
        {
            auto single = static_cast<float>(value);
            memcpy(value_in(property, container), &single, sizeof(single));
            return true;
        }
        return false;
    }

    auto is_real(FProperty* property) -> bool
    {
        return CastField<FDoubleProperty>(property) || CastField<FFloatProperty>(property);
    }

    // X and Y both float or double: anything else would leave them zero and the panel at the top left.
    auto writes_xy(FProperty* vector) -> bool
    {
        return CastField<FStructProperty>(vector) && is_real(member(vector, L"X")) && is_real(member(vector, L"Y"));
    }

    auto write_xy(FProperty* vector, void* container, double x, double y) -> void
    {
        auto* at = value_in(vector, container);
        write_real(member(vector, L"X"), at, x);
        write_real(member(vector, L"Y"), at, y);
    }

    auto write_color(FProperty* color, void* container, const double (&rgba)[4]) -> bool
    {
        if (!color) return false;
        auto* at = value_in(color, container);
        bool ok = true;
        const wchar_t* channels[4]{L"R", L"G", L"B", L"A"};
        for (int i = 0; i < 4; ++i) ok = write_real(member(color, channels[i]), at, rgba[i]) && ok;
        return ok;
    }

    auto sized(UFunction* function) -> bool
    {
        return function && function->GetPropertiesSize() >= 0 && static_cast<size_t>(function->GetPropertiesSize()) <= PARAMS;
    }

    auto call(UObject* object, UFunction* function, uint8_t* params) -> void
    {
        object->ProcessEvent(function, params);
    }
} // namespace

    auto DebugOverlay::resolve() -> void
    {
        m_state = -1;
        auto function = [](const wchar_t* path) { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); };
        auto type = [](const wchar_t* path) { return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path); };
        m_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        m_create = function(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        m_set_content = function(STR("/Script/UMG.ContentWidget:SetContent"));
        m_set_text = function(STR("/Script/UMG.TextBlock:SetText"));
        m_add = function(STR("/Script/UMG.UserWidget:AddToViewport"));
        m_remove = function(STR("/Script/UMG.Widget:RemoveFromParent"));
        m_set_position = function(STR("/Script/UMG.UserWidget:SetPositionInViewport"));
        m_set_anchors = function(STR("/Script/UMG.UserWidget:SetAnchorsInViewport"));
        m_set_alignment = function(STR("/Script/UMG.UserWidget:SetAlignmentInViewport"));
        m_user_widget = type(STR("/Script/UMG.UserWidget"));
        m_border = type(STR("/Script/UMG.Border"));
        m_text_block = type(STR("/Script/UMG.TextBlock"));
        auto* tree = type(STR("/Script/UMG.WidgetTree"));

        m_create_world = pointer_at(m_create, STR("WorldContextObject"));
        m_create_type = pointer_at(m_create, STR("WidgetType"));
        m_create_player = pointer_at(m_create, STR("OwningPlayer"));
        m_create_return = pointer_at(m_create, STR("ReturnValue"));
        m_widget_tree = pointer_at(m_user_widget, STR("WidgetTree"));
        m_root_widget = pointer_at(tree, STR("RootWidget"));
        m_content = pointer_at(m_set_content, STR("Content"));
        auto* text = find_property(m_set_text, STR("InText"));
        m_text_param = text && text->GetElementSize() >= FText::StaticSize() ? text->GetOffset_ForInternal() : -1;
        auto* z_order = find_property(m_add, STR("ZOrder"));
        m_z_order = z_order && z_order->GetElementSize() == static_cast<int32_t>(sizeof(int32_t)) ? z_order->GetOffset_ForInternal() : -1;
        m_position = find_property(m_set_position, STR("Position"));
        m_anchors = find_property(m_set_anchors, STR("Anchors"));
        m_alignment = find_property(m_set_alignment, STR("Alignment"));
        m_visibility = find_property(m_user_widget, STR("Visibility")); // UWidget's, on every widget here
        if (m_visibility && m_visibility->GetElementSize() != 1) m_visibility = nullptr;

        bool ok = m_library && sized(m_create) && sized(m_set_content) && sized(m_set_text) && sized(m_add) && m_remove &&
                  sized(m_set_position) && sized(m_set_anchors) && sized(m_set_alignment) && m_user_widget && m_border && m_text_block &&
                  m_create_world >= 0 && m_create_type >= 0 && m_create_player >= 0 && m_create_return >= 0 && m_widget_tree >= 0 &&
                  m_root_widget >= 0 && m_content >= 0 && m_text_param >= 0 && m_z_order >= 0 && writes_xy(m_position) &&
                  writes_xy(member(m_anchors, L"Minimum")) && writes_xy(member(m_anchors, L"Maximum")) && writes_xy(m_alignment) &&
                  m_visibility;
        if (!ok)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] UMG classes, functions or parameter layouts not as expected, debug overlay off\n"));
            return;
        }

        m_is_in_viewport = function(STR("/Script/UMG.Widget:IsInViewport"));
        auto* in_viewport_return = find_property(m_is_in_viewport, STR("ReturnValue"));
        m_in_viewport_return = sized(m_is_in_viewport) && in_viewport_return && in_viewport_return->GetElementSize() == 1
                                       ? in_viewport_return->GetOffset_ForInternal()
                                       : -1;
        m_brush_color = find_property(m_border, STR("BrushColor"));
        m_padding = find_property(m_border, STR("Padding"));
        m_font = find_property(m_text_block, STR("Font"));
        m_text_color = find_property(m_text_block, STR("ColorAndOpacity"));
        std::wstring missing;
        auto need = [&](bool found, const wchar_t* name) {
            if (!found) missing += (missing.empty() ? L"" : L", ") + std::wstring(name);
        };
        need(member(m_brush_color, L"R") != nullptr, L"Border.BrushColor");
        need(member(m_padding, L"Left") != nullptr, L"Border.Padding");
        need(member(m_font, L"Size") != nullptr, L"TextBlock.Font.Size");
        need(member(member(m_text_color, L"SpecifiedColor"), L"R") != nullptr, L"TextBlock.ColorAndOpacity");
        if (!missing.empty())
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] debug overlay: {} not found, left at the engine default\n"), missing);
        }
        m_state = 1;
    }

    auto DebugOverlay::build(UObject* controller, std::wstring text) -> bool
    {
        auto fail = [&](const wchar_t* why) {
            if (!m_build_warned) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] debug overlay not built: {}; retrying\n"), why);
            m_build_warned = true;
            return false;
        };
        alignas(16) uint8_t params[PARAMS]{};
        memcpy(params + m_create_world, &controller, sizeof(controller));
        memcpy(params + m_create_type, &m_user_widget, sizeof(m_user_widget));
        memcpy(params + m_create_player, &controller, sizeof(controller));
        call(m_library, m_create, params);
        UObject* widget = nullptr;
        memcpy(&widget, params + m_create_return, sizeof(widget));
        if (!widget) return fail(STR("Create returned nothing"));
        auto* tree = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(widget) + m_widget_tree);
        if (!tree) return fail(STR("the widget has no WidgetTree"));
        auto* border = UObjectGlobals::StaticConstructObject(FStaticConstructObjectParameters(m_border, tree));
        auto* label = UObjectGlobals::StaticConstructObject(FStaticConstructObjectParameters(m_text_block, tree));
        if (!border || !label) return fail(STR("a Border or TextBlock could not be constructed"));

        for (auto* object : {widget, border, label}) *value_in(m_visibility, object) = HIT_TEST_INVISIBLE;
        write_color(m_brush_color, border, BACKGROUND);
        if (member(m_padding, L"Left"))
        {
            for (auto* side : {L"Left", L"Top", L"Right", L"Bottom"}) write_real(member(m_padding, side), value_in(m_padding, border), PADDING);
        }
        if (m_font) write_real(member(m_font, L"Size"), value_in(m_font, label), FONT_SIZE);
        if (m_text_color) write_color(member(m_text_color, L"SpecifiedColor"), value_in(m_text_color, label), TEXT_COLOR);

        // RebuildWidget reads RootWidget, so it is set before AddToViewport.
        *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(tree) + m_root_widget) = border;
        memset(params, 0, sizeof(params));
        memcpy(params + m_content, &label, sizeof(label));
        call(border, m_set_content, params);
        m_widget = LiveRef::of(widget);
        m_text = LiveRef::of(label);
        set_text(std::move(text));

        // The viewport slot, before the widget is added: SetPositionInViewport resets the anchors to (0, 0), so
        // it goes first. Point anchors with a zero size make the slot size to the panel (GameViewportSubsystem).
        memset(params, 0, sizeof(params)); // bRemoveDPIScale false: Position is in slate units
        write_xy(m_position, params, -INSET, INSET);
        call(widget, m_set_position, params);
        memset(params, 0, sizeof(params));
        write_xy(member(m_anchors, L"Minimum"), value_in(m_anchors, params), 1.0, 0.0);
        write_xy(member(m_anchors, L"Maximum"), value_in(m_anchors, params), 1.0, 0.0);
        call(widget, m_set_anchors, params);
        memset(params, 0, sizeof(params));
        write_xy(m_alignment, params, 1.0, 0.0);
        call(widget, m_set_alignment, params);
        memset(params, 0, sizeof(params));
        memcpy(params + m_z_order, &Z_ORDER, sizeof(Z_ORDER));
        call(widget, m_add, params);
        // AddToViewport returns nothing and can refuse (no viewport subsystem, a world being torn down). Checked
        // here, so a refusal is a failed build, retried at the RETRY cadence and warned once, and not a "shown"
        // panel that the refresh then finds missing and rebuilds.
        if (!in_viewport())
        {
            forget(); // never on screen; the widget is unreferenced and goes at the next GC
            return fail(STR("AddToViewport did not add it"));
        }

        m_build_warned = false;
        if (dw::verbose()) Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] debug overlay shown\n"));
        return true;
    }

    auto DebugOverlay::set_text(std::wstring value) -> void
    {
        FText text(value.c_str());
        alignas(16) uint8_t params[PARAMS]{};
        text.CopyBorrowedTo(params + m_text_param);
        call(m_text.object, m_set_text, params);
        m_shown = std::move(value);
    }

    auto DebugOverlay::in_viewport() -> bool
    {
        if (m_in_viewport_return < 0) return true;
        alignas(16) uint8_t params[PARAMS]{};
        call(m_widget.object, m_is_in_viewport, params);
        return params[m_in_viewport_return] != 0;
    }

    auto DebugOverlay::hide() -> void
    {
        if (!m_widget.object) return;
        if (m_widget.alive())
        {
            alignas(16) uint8_t params[PARAMS]{};
            call(m_widget.object, m_remove, params);
        }
        forget();
    }
} // namespace dw::smoothwalker::ui
