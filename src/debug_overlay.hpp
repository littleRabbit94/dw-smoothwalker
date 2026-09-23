// Debug overlay (debug_overlay, debug_key): a UMG text panel at the top right of the screen with the live follow,
// the player's camera modes, the last hard cut and the camera API claim (docs/design.md, "Debug overlay").
// Built, refreshed and removed on the game thread only, from the engine tick. The GetCameraView hook never touches
// it: it publishes numbers (dllmain.cpp, g_debug_*) that the refresh reads.
//
// Included from dllmain.cpp after lua_api.hpp.
#pragma once

#include "config.hpp"
#include "lua_api.hpp"
#include "mode_tuning.hpp"
#include "smoothing.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dwsc
{
    using namespace RC;
    using namespace RC::Unreal;

    // The camera API as the panel shows it.
    struct ApiStatus
    {
        std::wstring owner; // empty: nobody holds the camera
        double lease = NAN; // s left on the owner's lease; NAN: no lease
        bool glide = false; // the hook is crossfading back after a release("glide")
    };

    // Read-only, the way Smoothwalker.owner() reads it: under dwapi::g_mutex, and an expired lease reads as nobody.
    // The drop itself stays with claim and release. glide is the caller's (a hook atomic).
    inline auto api_status() -> ApiStatus
    {
        ApiStatus s;
        std::string mod;
        int64_t expires = 0;
        {
            std::lock_guard guard(dwapi::g_mutex);
            expires = dwapi::g_owner_expires.load(std::memory_order_relaxed);
            if (dwapi::g_owner_state && (expires == 0 || dwapi::qpc_now() < expires)) mod = dwapi::g_owner_mod;
        }
        if (mod.empty()) return s;
        s.owner = to_wide(mod);
        if (expires != 0) s.lease = std::max(0.0, static_cast<double>(expires - dwapi::qpc_now()) / dwapi::qpc_frequency());
        return s;
    }

    // What the panel shows, gathered on the game thread at a refresh. NAN: not known (the follow is off, or has not
    // run on this camera yet).
    struct DebugPanel
    {
        bool enabled = false;
        std::wstring preset;
        bool camera_tuning = false;
        std::wstring camera_type;
        std::vector<ModeListing> modes; // newest first
        bool modes_stale = false;       // a rescan is pending, the list may be short
        double keep_follow = NAN;       // share of the trail shown, after the aiming, combat and traversal blends
        double keep_turn = NAN;         // share of the turning smoothing shown
        bool rotation_smoothing = false;
        Influence influence = Influence::None;
        double lag_h = NAN, lag_v = NAN; // cm of lag on screen
        double max_lag_h = 0.0, max_lag_v = 0.0;
        double rate_h = NAN; // 1/s, the horizontal follow rate after the curve
        Snap snap = Snap::None;
        double snap_age = NAN; // s since that snap
        ApiStatus api;
    };

    inline auto snap_name(Snap snap) -> const wchar_t*
    {
        switch (snap)
        {
        case Snap::Startup: return L"startup";
        case Snap::Teleport: return L"teleport";
        case Snap::Gap: return L"gap";
        case Snap::World: return L"world change";
        case Snap::Player: return L"player change";
        case Snap::Pawn: return L"pawn change";
        case Snap::Toggle: return L"toggle";
        case Snap::ApiCut: return L"api cut";
        case Snap::ClaimEnded: return L"api lease ended";
        case Snap::ViewLost: return L"view lost";
        default: return L"none";
        }
    }

    inline auto influence_name(Influence influence) -> const wchar_t*
    {
        switch (influence)
        {
        case Influence::Traversal: return L"traversal";
        case Influence::Combat: return L"combat";
        case Influence::Aiming: return L"aiming";
        default: return L"none";
        }
    }

    // One value per line. Roboto is proportional, so the padding lines columns up only roughly.
    inline auto format_panel(const DebugPanel& p) -> std::wstring
    {
        constexpr size_t MAX_MODES = 8;
        constexpr const wchar_t* INDENT = L"              ";
        auto number = [](double v, const wchar_t* unit) -> std::wstring {
            return std::isfinite(v) ? std::format(L"{:.0f}{}", v, unit) : std::wstring(L"-");
        };

        std::wstring out = std::format(L"Smoothwalker  {:<5}preset: {}   tuning: {}\n", p.enabled ? L"ON" : L"OFF", p.preset,
                                       p.camera_tuning ? L"on" : L"off");
        out += std::format(L"camera type   {}\n", p.camera_type.empty() ? std::wstring(L"?") : p.camera_type);

        if (p.modes.empty()) out += L"modes         none\n";
        for (size_t i = 0; i < p.modes.size() && i < MAX_MODES; ++i)
        {
            const auto& m = p.modes[i];
            const wchar_t* state = m.state == 0 ? L"blend in" : m.state == 1 ? L"active" : L"blend out";
            auto tag = m.group < 0 ? std::wstring(L"[untracked]")
                                   : std::format(L"[{}, {}]", group_name(static_cast<Group>(m.group)), m.tuned ? L"tuned" : L"as shipped");
            out += std::format(L"{}{:<20}{:<11}{}\n", i == 0 ? L"modes         " : INDENT, m.name, state, tag);
        }
        if (p.modes.size() > MAX_MODES) out += std::format(L"{}+{} more\n", INDENT, p.modes.size() - MAX_MODES);
        if (p.modes_stale) out += std::format(L"{}(rescan pending)\n", INDENT);

        auto turn = !p.rotation_smoothing ? std::wstring(L"off") : number(p.keep_turn * 100.0, L"%");
        out += std::format(L"follow {}   turn {}", number(p.keep_follow * 100.0, L"%"), turn);
        out += std::isfinite(p.keep_follow) ? std::format(L"   ({})\n", influence_name(p.influence)) : std::wstring(L"\n");
        out += std::format(L"lag h {}/{:.0f} cm   v {}/{:.0f} cm   rate {}\n", number(p.lag_h, L""), p.max_lag_h, number(p.lag_v, L""), p.max_lag_v,
                           std::isfinite(p.rate_h) ? std::format(L"{:.1f}/s", p.rate_h) : std::wstring(L"-"));

        if (p.snap == Snap::None || !std::isfinite(p.snap_age)) out += L"last snap     none yet\n";
        else out += std::format(L"last snap     {}, {:.0f} s ago\n", snap_name(p.snap), p.snap_age);

        if (!p.api.owner.empty())
        {
            out += std::isfinite(p.api.lease) ? std::format(L"api           {} lease {:.1f} s", p.api.owner, p.api.lease)
                                              : std::format(L"api           {} (no lease)", p.api.owner);
        }
        else out += p.api.glide ? L"api           glide" : L"api           none";
        return out;
    }

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
        static constexpr size_t PARAMS = 256;                  // every params buffer here; checked against reflection
        static constexpr int32_t Z_ORDER = 1000;
        static constexpr double INSET = 16.0;         // slate units from the right and top edges
        static constexpr uint8_t HIT_TEST_INVISIBLE = 3; // ESlateVisibility: drawn, never takes input
        static constexpr double FONT_SIZE = 22.0;
        static constexpr double PADDING = 8.0;
        static constexpr double BACKGROUND[4]{0.0, 0.0, 0.0, 0.6};
        static constexpr double TEXT_COLOR[4]{0.92, 0.92, 0.88, 1.0};

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

        static auto find_property(UStruct* owner, const wchar_t* name) -> FProperty*
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
        static auto member(FProperty* of, const wchar_t* name) -> FProperty*
        {
            auto* as_struct = CastField<FStructProperty>(of);
            return as_struct ? find_property(as_struct->GetStruct().Get(), name) : nullptr;
        }

        // The offset of a pointer-sized parameter or property, or -1.
        static auto pointer_at(UStruct* owner, const wchar_t* name) -> int32_t
        {
            auto* property = find_property(owner, name);
            return property && property->GetElementSize() == static_cast<int32_t>(sizeof(void*)) ? property->GetOffset_ForInternal() : -1;
        }

        static auto value_in(FProperty* property, void* container) -> uint8_t*
        {
            return static_cast<uint8_t*>(container) + property->GetOffset_ForInternal();
        }

        // A float or a double property; false for anything else, which is left alone.
        static auto write_real(FProperty* property, void* container, double value) -> bool
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

        static auto is_real(FProperty* property) -> bool
        {
            return CastField<FDoubleProperty>(property) || CastField<FFloatProperty>(property);
        }

        // X and Y both float or double: anything else would leave them zero and the panel at the top left.
        static auto writes_xy(FProperty* vector) -> bool
        {
            return CastField<FStructProperty>(vector) && is_real(member(vector, L"X")) && is_real(member(vector, L"Y"));
        }

        static auto write_xy(FProperty* vector, void* container, double x, double y) -> void
        {
            auto* at = value_in(vector, container);
            write_real(member(vector, L"X"), at, x);
            write_real(member(vector, L"Y"), at, y);
        }

        static auto write_color(FProperty* color, void* container, const double (&rgba)[4]) -> bool
        {
            if (!color) return false;
            auto* at = value_in(color, container);
            bool ok = true;
            const wchar_t* channels[4]{L"R", L"G", L"B", L"A"};
            for (int i = 0; i < 4; ++i) ok = write_real(member(color, channels[i]), at, rgba[i]) && ok;
            return ok;
        }

        static auto sized(UFunction* function) -> bool
        {
            return function && function->GetPropertiesSize() >= 0 && static_cast<size_t>(function->GetPropertiesSize()) <= PARAMS;
        }

        // Once, the first time the panel is wanted. Everything that places or feeds the panel is required; the
        // styling is not.
        auto resolve() -> void
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

        auto call(UObject* object, UFunction* function, uint8_t* params) -> void
        {
            object->ProcessEvent(function, params);
        }

        auto build(UObject* controller, std::wstring text) -> bool
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
            if (g_log_verbose.load(std::memory_order_relaxed)) Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] debug overlay shown\n"));
            return true;
        }

        auto set_text(std::wstring value) -> void
        {
            FText text(value.c_str());
            alignas(16) uint8_t params[PARAMS]{};
            text.CopyBorrowedTo(params + m_text_param);
            call(m_text.object, m_set_text, params);
            m_shown = std::move(value);
        }

        // True unless Widget:IsInViewport says the panel is off the screen (true when that function is missing).
        auto in_viewport() -> bool
        {
            if (m_in_viewport_return < 0) return true;
            alignas(16) uint8_t params[PARAMS]{};
            call(m_widget.object, m_is_in_viewport, params);
            return params[m_in_viewport_return] != 0;
        }

        // The widget leaves the viewport and is collected; switched on again, a new one is built.
        auto hide() -> void
        {
            if (!m_widget.object) return;
            if (m_widget.alive())
            {
                alignas(16) uint8_t params[PARAMS]{};
                call(m_widget.object, m_remove, params);
            }
            forget();
        }
    };
} // namespace dwsc
