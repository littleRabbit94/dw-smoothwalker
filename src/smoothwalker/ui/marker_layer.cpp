// The debug markers' widget (ui/marker_layer.hpp): reflection lookups, the canvas and its Borders built once, and the
// per-tick sends of what the layout (ui/markers.hpp) changed.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "marker_layer.hpp"
#include "umg.hpp"
#include "../../common/log.hpp"

#include <cmath>
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
    using namespace umg;

namespace
{
    constexpr int32_t Z_ORDER = 999;          // under the debug overlay's panel (1000)
    constexpr uint8_t COLLAPSED = 1;          // ESlateVisibility: not drawn, no space
    constexpr uint8_t HIT_TEST_INVISIBLE = 3; // drawn, never takes input
    constexpr uint8_t ROUNDED_BOX = 4;        // ESlateBrushDrawType::RoundedBox
    constexpr uint8_t HALF_HEIGHT_RADIUS = 1; // ESlateBrushRoundingType::HalfHeightRadius

    auto rgba(markers::Rgba c, double (&out)[4]) -> const double (&)[4]
    {
        out[0] = c.r;
        out[1] = c.g;
        out[2] = c.b;
        out[3] = c.a;
        return out;
    }

    // A one-byte property (an enum as a byte), or null.
    auto byte_property(FProperty* property) -> FProperty*
    {
        return property && property->GetElementSize() == 1 ? property : nullptr;
    }

    // The offset of a one-byte parameter or property, or -1.
    auto byte_at(UStruct* owner, const wchar_t* name) -> int32_t
    {
        auto* property = byte_property(find_property(owner, name));
        return property ? property->GetOffset_ForInternal() : -1;
    }

    // A struct property whose three named members are all float or double.
    auto reals3(FProperty* of, const wchar_t* a, const wchar_t* b, const wchar_t* c) -> bool
    {
        return is_real(member(of, a)) && is_real(member(of, b)) && is_real(member(of, c));
    }

    // A struct property's named members, looked up once (resolve).
    auto members(FProperty* of, const wchar_t* a, const wchar_t* b, FProperty* (&out)[2]) -> void
    {
        out[0] = member(of, a);
        out[1] = member(of, b);
    }

    auto members(FProperty* of, const wchar_t* a, const wchar_t* b, const wchar_t* c, FProperty* (&out)[3]) -> void
    {
        out[0] = member(of, a);
        out[1] = member(of, b);
        out[2] = member(of, c);
    }

    // The struct at `container` read through its members found once, into out; false (out untouched) when any is
    // not a number.
    template <size_t N>
    auto read_members(FProperty* of, FProperty* const (&fields)[N], const uint8_t* container, double (&out)[N]) -> bool
    {
        const auto* at = container + of->GetOffset_ForInternal();
        double v[N]{};
        for (size_t i = 0; i < N; ++i)
            if (!read_real(fields[i], at, v[i])) return false;
        for (size_t i = 0; i < N; ++i) out[i] = v[i];
        return true;
    }

    // Slate units to 0.5 and degrees to 0.1 before the change check: sub-pixel jitter sends nothing.
    auto half_unit(double v) -> double { return std::round(v * 2.0) / 2.0; }
    auto tenth(double v) -> double { return std::round(v * 10.0) / 10.0; }
} // namespace

    auto MarkerLayer::resolve() -> void
    {
        m_state = -1;
        auto function = [](const wchar_t* path) { return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); };
        auto type = [](const wchar_t* path) { return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path); };
        m_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
        m_layout_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetLayoutLibrary"));
        m_create = function(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
        m_add = function(STR("/Script/UMG.UserWidget:AddToViewport"));
        m_remove = function(STR("/Script/UMG.Widget:RemoveFromParent"));
        m_add_child = function(STR("/Script/UMG.CanvasPanel:AddChildToCanvas"));
        m_slot_position = function(STR("/Script/UMG.CanvasPanelSlot:SetPosition"));
        m_slot_size = function(STR("/Script/UMG.CanvasPanelSlot:SetSize"));
        m_slot_alignment = function(STR("/Script/UMG.CanvasPanelSlot:SetAlignment"));
        m_slot_auto_size = function(STR("/Script/UMG.CanvasPanelSlot:SetAutoSize"));
        m_set_angle = function(STR("/Script/UMG.Widget:SetRenderTransformAngle"));
        m_set_opacity = function(STR("/Script/UMG.Widget:SetRenderOpacity"));
        m_set_visibility = function(STR("/Script/UMG.Widget:SetVisibility"));
        m_set_text = function(STR("/Script/UMG.TextBlock:SetText"));
        m_viewport_size = function(STR("/Script/UMG.WidgetLayoutLibrary:GetViewportSize"));
        m_viewport_scale = function(STR("/Script/UMG.WidgetLayoutLibrary:GetViewportScale"));
        m_user_widget = type(STR("/Script/UMG.UserWidget"));
        m_canvas_panel = type(STR("/Script/UMG.CanvasPanel"));
        m_border = type(STR("/Script/UMG.Border"));
        m_text_block = type(STR("/Script/UMG.TextBlock"));
        auto* tree = type(STR("/Script/UMG.WidgetTree"));

        m_create_world = pointer_at(m_create, STR("WorldContextObject"));
        m_create_type = pointer_at(m_create, STR("WidgetType"));
        m_create_player = pointer_at(m_create, STR("OwningPlayer"));
        m_create_return = pointer_at(m_create, STR("ReturnValue"));
        m_widget_tree = pointer_at(m_user_widget, STR("WidgetTree"));
        m_root_widget = pointer_at(tree, STR("RootWidget"));
        auto* z_order = find_property(m_add, STR("ZOrder"));
        m_z_order = z_order && z_order->GetElementSize() == static_cast<int32_t>(sizeof(int32_t)) ? z_order->GetOffset_ForInternal() : -1;
        m_child_content = pointer_at(m_add_child, STR("Content"));
        m_child_return = pointer_at(m_add_child, STR("ReturnValue"));
        m_position = find_property(m_slot_position, STR("InPosition"));
        m_size = find_property(m_slot_size, STR("InSize"));
        m_alignment = find_property(m_slot_alignment, STR("InAlignment"));
        m_auto_size = CastField<FBoolProperty>(find_property(m_slot_auto_size, STR("InbAutoSize")));
        m_angle = find_property(m_set_angle, STR("Angle"));
        m_opacity = find_property(m_set_opacity, STR("InOpacity"));
        m_visibility_param = byte_at(m_set_visibility, STR("InVisibility"));
        auto* text = find_property(m_set_text, STR("InText"));
        m_text_param = text && text->GetElementSize() >= FText::StaticSize() ? text->GetOffset_ForInternal() : -1;
        m_size_world = pointer_at(m_viewport_size, STR("WorldContextObject"));
        m_size_return = find_property(m_viewport_size, STR("ReturnValue"));
        m_scale_world = pointer_at(m_viewport_scale, STR("WorldContextObject"));
        m_scale_return = find_property(m_viewport_scale, STR("ReturnValue"));
        m_visibility = byte_property(find_property(m_user_widget, STR("Visibility"))); // UWidget's, on every widget here
        members(m_position, STR("X"), STR("Y"), m_position_xy);
        members(m_size, STR("X"), STR("Y"), m_size_xy);
        members(m_alignment, STR("X"), STR("Y"), m_alignment_xy);
        members(m_size_return, STR("X"), STR("Y"), m_size_return_xy);

        bool ok = m_library && m_layout_library && sized(m_create) && sized(m_add) && m_remove && sized(m_add_child) && sized(m_slot_position) &&
                  sized(m_slot_size) && sized(m_slot_alignment) && sized(m_slot_auto_size) && sized(m_set_angle) && sized(m_set_opacity) &&
                  sized(m_set_visibility) && sized(m_set_text) && sized(m_viewport_size) && sized(m_viewport_scale) && m_user_widget &&
                  m_canvas_panel && m_border && m_text_block && m_create_world >= 0 && m_create_type >= 0 && m_create_player >= 0 &&
                  m_create_return >= 0 && m_widget_tree >= 0 && m_root_widget >= 0 && m_z_order >= 0 && m_child_content >= 0 &&
                  m_child_return >= 0 && writes_xy(m_position) && writes_xy(m_size) && writes_xy(m_alignment) && m_auto_size &&
                  is_real(m_angle) && is_real(m_opacity) && m_visibility_param >= 0 && m_text_param >= 0 && m_size_world >= 0 &&
                  writes_xy(m_size_return) && m_scale_world >= 0 && is_real(m_scale_return) && m_visibility;
        if (!ok)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] UMG classes, functions or parameter layouts not as expected, debug markers off\n"));
            return;
        }

        m_is_in_viewport = function(STR("/Script/UMG.Widget:IsInViewport"));
        auto* in_viewport_return = find_property(m_is_in_viewport, STR("ReturnValue"));
        m_in_viewport_return = sized(m_is_in_viewport) && in_viewport_return && in_viewport_return->GetElementSize() == 1
                                       ? in_viewport_return->GetOffset_ForInternal()
                                       : -1;
        m_brush_color = find_property(m_border, STR("BrushColor"));
        m_background = find_property(m_border, STR("Background"));
        m_draw_as = byte_property(member(m_background, STR("DrawAs")));
        m_outline = member(m_background, STR("OutlineSettings"));
        m_rounding = byte_property(member(m_outline, STR("RoundingType")));
        m_outline_width = member(m_outline, STR("Width"));
        m_outline_color = member(m_outline, STR("Color"));
        m_outline_transparency = member(m_outline, STR("bUseBrushTransparency"));
        m_font = find_property(m_text_block, STR("Font"));
        m_text_color = find_property(m_text_block, STR("ColorAndOpacity"));
        std::wstring missing;
        auto need = [&](bool found, const wchar_t* name) {
            if (!found) missing += (missing.empty() ? L"" : L", ") + std::wstring(name);
        };
        need(member(m_brush_color, L"R") != nullptr, L"Border.BrushColor");
        need(m_draw_as != nullptr, L"Border.Background.DrawAs");
        need(m_rounding != nullptr, L"Border.Background.OutlineSettings.RoundingType");
        need(is_real(m_outline_width), L"Border.Background.OutlineSettings.Width");
        need(member(member(m_outline_color, L"SpecifiedColor"), L"R") != nullptr, L"Border.Background.OutlineSettings.Color");
        need(CastField<FBoolProperty>(m_outline_transparency) != nullptr, L"Border.Background.OutlineSettings.bUseBrushTransparency");
        need(member(m_font, L"Size") != nullptr, L"TextBlock.Font.Size");
        need(member(member(m_text_color, L"SpecifiedColor"), L"R") != nullptr, L"TextBlock.ColorAndOpacity");
        if (!missing.empty())
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] debug markers: {} not found, left at the engine default\n"), missing);
        }
        resolve_projection();
        m_state = 1;
    }

    // The settings that shape the projection (markers::Viewport) and the rendered view, found once. Settings missing:
    // the values measured in this game (AspectRatio 1.7778, MaintainYFOV, bConstrainAspectRatio false), with one
    // warning. The view missing: the feed's shown view, before camera modifiers, with one warning.
    auto MarkerLayer::resolve_projection() -> void
    {
        auto type = [](const wchar_t* path) { return UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path); };
        auto* controller = type(STR("/Script/Engine.PlayerController"));
        m_camera_manager_class = type(STR("/Script/Engine.PlayerCameraManager"));
        m_local_player_class = type(STR("/Script/Engine.LocalPlayer"));
        m_camera_manager_at = pointer_at(controller, STR("PlayerCameraManager"));
        m_player_at = pointer_at(controller, STR("Player"));
        auto* cache = find_property(m_camera_manager_class, STR("CameraCachePrivate"));
        auto* pov = member(cache, STR("POV"));
        m_pov_at = cache && pov ? cache->GetOffset_ForInternal() + pov->GetOffset_ForInternal() : -1;
        m_aspect_ratio = member(pov, STR("AspectRatio"));
        m_constrain = CastField<FBoolProperty>(member(pov, STR("bConstrainAspectRatio")));
        m_axis_at = byte_at(m_local_player_class, STR("AspectRatioAxisConstraint"));
        m_pov_location = member(pov, STR("Location"));
        m_pov_rotation = member(pov, STR("Rotation"));
        m_pov_fov = member(pov, STR("FOV"));
        members(m_pov_location, STR("X"), STR("Y"), STR("Z"), m_pov_location_xyz);
        members(m_pov_rotation, STR("Pitch"), STR("Yaw"), STR("Roll"), m_pov_rotation_pyr);
        const bool manager = m_camera_manager_class && m_camera_manager_at >= 0 && m_pov_at >= 0;
        m_projection_ok = manager && m_local_player_class && m_player_at >= 0 && is_real(m_aspect_ratio) && m_constrain && m_axis_at >= 0;
        m_pov_view_ok = manager && reals3(m_pov_location, STR("X"), STR("Y"), STR("Z")) &&
                        reals3(m_pov_rotation, STR("Pitch"), STR("Yaw"), STR("Roll")) && is_real(m_pov_fov);
        if (!m_projection_ok)
        {
            Output::send<LogLevel::Warning>(
                    STR("[DWSmoothwalker] debug markers: the camera's aspect ratio settings not found, projecting as 16:9 with the vertical FOV kept\n"));
        }
        if (!m_pov_view_ok)
        {
            Output::send<LogLevel::Warning>(
                    STR("[DWSmoothwalker] debug markers: the rendered camera view not found, projecting the view before camera shakes and FOV changes\n"));
        }
    }

    auto MarkerLayer::style(UObject* border, int index) -> void
    {
        double color[4]{};
        const markers::Look look = markers::look(index);
        if (look == markers::Look::Box || !m_background)
        {
            write_color(m_brush_color, border, rgba(markers::color(index), color));
            return;
        }
        auto* brush = value_in(m_background, border);
        if (m_draw_as) *value_in(m_draw_as, brush) = ROUNDED_BOX;
        auto* outline = m_outline ? value_in(m_outline, brush) : nullptr;
        if (outline && m_rounding) *value_in(m_rounding, outline) = HALF_HEIGHT_RADIUS;
        if (look == markers::Look::Dot)
        {
            write_color(m_brush_color, border, rgba(markers::color(index), color));
            return;
        }
        // A bead: the fill in the colour inside a dark rim (the outline draws inside the box).
        if (look == markers::Look::Bead)
        {
            if (outline)
            {
                write_real(m_outline_width, outline, markers::LEASH_RIM);
                if (m_outline_color) write_color(member(m_outline_color, L"SpecifiedColor"), value_in(m_outline_color, outline), rgba(markers::RIM, color));
                write_bool(m_outline_transparency, outline, false);
            }
            write_color(m_brush_color, border, rgba(markers::color(index), color));
            return;
        }
        // A ring: the outline in the colour, the fill clear, the outline kept whatever the fill's alpha.
        if (outline)
        {
            write_real(m_outline_width, outline, markers::RING_OUTLINE);
            if (m_outline_color) write_color(member(m_outline_color, L"SpecifiedColor"), value_in(m_outline_color, outline), rgba(markers::color(index), color));
            write_bool(m_outline_transparency, outline, false);
        }
        write_color(m_brush_color, border, rgba(markers::CLEAR, color));
    }

    auto MarkerLayer::build(UObject* controller) -> bool
    {
        auto fail = [&](const wchar_t* why) {
            forget(); // never on screen; whatever was built is unreferenced and goes at the next GC
            if (!m_build_warned) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] debug markers not built: {}; retrying\n"), why);
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
        auto* canvas = UObjectGlobals::StaticConstructObject(FStaticConstructObjectParameters(m_canvas_panel, tree));
        if (!canvas) return fail(STR("a CanvasPanel could not be constructed"));
        for (auto* object : {widget, canvas}) *value_in(m_visibility, object) = HIT_TEST_INVISIBLE;
        // RebuildWidget reads RootWidget, so it is set before AddToViewport. With the canvas slot's default anchors the
        // canvas fills the viewport.
        *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(tree) + m_root_widget) = canvas;
        m_widget = LiveRef::of(widget);
        m_canvas = LiveRef::of(canvas);

        // Every child centred on its slot's position (alignment 0.5, 0.5), from the canvas's top left (point anchors
        // at 0, 0), collapsed until the first tick that shows it.
        auto add = [&](UObject* child) -> UObject* {
            *value_in(m_visibility, child) = COLLAPSED;
            alignas(16) uint8_t add_params[PARAMS]{};
            memcpy(add_params + m_child_content, &child, sizeof(child));
            call(canvas, m_add_child, add_params);
            UObject* slot = nullptr;
            memcpy(&slot, add_params + m_child_return, sizeof(slot));
            if (slot) set_xy(slot, m_slot_alignment, m_alignment, m_alignment_xy, 0.5, 0.5);
            return slot;
        };
        for (int i = 0; i < markers::COUNT; ++i)
        {
            auto* border = UObjectGlobals::StaticConstructObject(FStaticConstructObjectParameters(m_border, tree));
            if (!border) return fail(STR("a Border could not be constructed"));
            style(border, i);
            auto* slot = add(border);
            if (!slot) return fail(STR("AddChildToCanvas returned no slot"));
            m_shapes[i].widget = LiveRef::of(border);
            m_shapes[i].slot = LiveRef::of(slot);
        }
        auto* label = UObjectGlobals::StaticConstructObject(FStaticConstructObjectParameters(m_text_block, tree));
        if (!label) return fail(STR("a TextBlock could not be constructed"));
        double color[4]{};
        if (m_font) write_real(member(m_font, L"Size"), value_in(m_font, label), markers::LABEL_FONT);
        if (m_text_color) write_color(member(m_text_color, L"SpecifiedColor"), value_in(m_text_color, label), rgba(markers::LINE, color));
        auto* label_slot = add(label);
        if (!label_slot) return fail(STR("AddChildToCanvas returned no slot"));
        memset(params, 0, sizeof(params));
        write_bool(m_auto_size, params, true); // the slot takes the text's own size
        call(label_slot, m_slot_auto_size, params);
        m_label.widget = LiveRef::of(label);
        m_label.slot = LiveRef::of(label_slot);

        memset(params, 0, sizeof(params));
        memcpy(params + m_z_order, &Z_ORDER, sizeof(Z_ORDER));
        call(widget, m_add, params);
        // AddToViewport returns nothing and can refuse (no viewport subsystem, a world being torn down): a refusal is a
        // failed build, retried at the RETRY cadence and warned once.
        if (!in_viewport()) return fail(STR("AddToViewport did not add it"));

        m_root_shown = true;
        m_build_warned = false;
        if (dw::verbose()) Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] debug markers shown\n"));
        return true;
    }

    auto MarkerLayer::alive() const -> bool
    {
        if (!m_widget.alive() || !m_canvas.alive() || !m_label.widget.alive() || !m_label.slot.alive()) return false;
        for (const auto& shape : m_shapes)
            if (!shape.widget.alive() || !shape.slot.alive()) return false;
        return true;
    }

    auto MarkerLayer::in_viewport() -> bool
    {
        if (m_in_viewport_return < 0) return true;
        alignas(16) uint8_t params[PARAMS]{};
        call(m_widget.object, m_is_in_viewport, params);
        return params[m_in_viewport_return] != 0;
    }

    // The viewport's size and scale, and the projection settings and the rendered view from the player's camera
    // manager and local player, read this tick. What cannot be read keeps markers::Viewport's defaults (a zero size
    // hides the markers) and leaves scene.has_camera false.
    auto MarkerLayer::viewport(UObject* controller, markers::Scene& scene) -> markers::Viewport
    {
        markers::Viewport out;
        alignas(16) uint8_t params[PARAMS]{};
        memcpy(params + m_size_world, &controller, sizeof(controller));
        call(m_layout_library, m_viewport_size, params);
        double size[2]{};
        if (read_members(m_size_return, m_size_return_xy, params, size))
        {
            out.width = size[0];
            out.height = size[1];
        }
        memset(params, 0, sizeof(params));
        memcpy(params + m_scale_world, &controller, sizeof(controller));
        call(m_layout_library, m_viewport_scale, params);
        read_real(m_scale_return, params, out.scale);
        if (!m_projection_ok && !m_pov_view_ok) return out;

        auto* bytes = reinterpret_cast<uint8_t*>(controller);
        auto* manager = *reinterpret_cast<UObject**>(bytes + m_camera_manager_at);
        if (manager && manager->IsA(m_camera_manager_class))
        {
            const auto* pov = reinterpret_cast<const uint8_t*>(manager) + m_pov_at;
            if (m_projection_ok)
            {
                double aspect = 0.0;
                if (read_real(m_aspect_ratio, pov, aspect) && std::isfinite(aspect) && aspect > 0.0) out.aspect_ratio = aspect;
                read_bool(m_constrain, pov, out.constrain_aspect);
            }
            double fov = NAN;
            if (m_pov_view_ok && read_members(m_pov_location, m_pov_location_xyz, pov, scene.camera.location) &&
                read_members(m_pov_rotation, m_pov_rotation_pyr, pov, scene.camera.rotation) && read_real(m_pov_fov, pov, fov))
            {
                scene.camera.fov = fov;
                scene.has_camera = true;
            }
        }
        if (!m_projection_ok) return out;
        auto* player = *reinterpret_cast<UObject**>(bytes + m_player_at);
        if (player && player->IsA(m_local_player_class)) out.axis_constraint = reinterpret_cast<const uint8_t*>(player)[m_axis_at];
        return out;
    }

    auto MarkerLayer::draw(UObject* controller, markers::Scene scene) -> void
    {
        // The cheap gates first: a hidden layer costs the gather and nothing else.
        if (!markers::drawable(scene))
        {
            m_markers.clear();
            set_root_shown(false);
            return;
        }
        const markers::Viewport view = viewport(controller, scene);
        const markers::Layout layout = markers::layout(scene, view, m_markers);
        bool any = layout.label.visible;
        for (const auto& p : layout.shapes) any = any || p.visible;
        if (!any)
        {
            set_root_shown(false); // one call; the children keep their state for the next frame shown
            return;
        }
        for (int i = 0; i < markers::COUNT; ++i) apply(m_shapes[i], layout.shapes[i]);
        apply_label(layout.label);
        set_root_shown(true);
    }

    auto MarkerLayer::apply(Shape& shape, const markers::Primitive& p) -> void
    {
        if (!p.visible)
        {
            if (shape.shown) set_visibility(shape.widget.object, COLLAPSED);
            shape.shown = false;
            return;
        }
        const double x = half_unit(p.x), y = half_unit(p.y), w = half_unit(p.w), angle = tenth(p.angle_deg);
        if (x != shape.x || y != shape.y)
        {
            set_xy(shape.slot.object, m_slot_position, m_position, m_position_xy, x, y);
            shape.x = x;
            shape.y = y;
        }
        if (w != shape.w || p.h != shape.h)
        {
            set_xy(shape.slot.object, m_slot_size, m_size, m_size_xy, w, p.h);
            shape.w = w;
            shape.h = p.h;
        }
        if (angle != shape.angle)
        {
            set_real(shape.widget.object, m_set_angle, m_angle, angle);
            shape.angle = angle;
        }
        if (p.opacity != shape.opacity)
        {
            set_real(shape.widget.object, m_set_opacity, m_opacity, p.opacity);
            shape.opacity = p.opacity;
        }
        if (!shape.shown) set_visibility(shape.widget.object, HIT_TEST_INVISIBLE);
        shape.shown = true;
    }

    auto MarkerLayer::apply_label(const markers::Label& label) -> void
    {
        if (!label.visible)
        {
            if (m_label.shown) set_visibility(m_label.widget.object, COLLAPSED);
            m_label.shown = false;
            return;
        }
        const double x = half_unit(label.x), y = half_unit(label.y);
        if (x != m_label.x || y != m_label.y)
        {
            set_xy(m_label.slot.object, m_slot_position, m_position, m_position_xy, x, y);
            m_label.x = x;
            m_label.y = y;
        }
        if (label.cm != m_label_cm)
        {
            const std::wstring value = markers::label_text(label.cm);
            FText text(value.c_str());
            alignas(16) uint8_t params[PARAMS]{};
            text.CopyBorrowedTo(params + m_text_param);
            call(m_label.widget.object, m_set_text, params);
            m_label_cm = label.cm;
        }
        if (!m_label.shown) set_visibility(m_label.widget.object, HIT_TEST_INVISIBLE);
        m_label.shown = true;
    }

    auto MarkerLayer::set_root_shown(bool shown) -> void
    {
        if (shown == m_root_shown) return;
        set_visibility(m_widget.object, shown ? HIT_TEST_INVISIBLE : COLLAPSED);
        m_root_shown = shown;
    }

    auto MarkerLayer::set_visibility(UObject* widget, uint8_t visibility) -> void
    {
        alignas(16) uint8_t params[PARAMS]{};
        params[m_visibility_param] = visibility;
        call(widget, m_set_visibility, params);
    }

    auto MarkerLayer::set_xy(UObject* object, UFunction* function, FProperty* parameter, FProperty* const (&xy)[2], double x, double y) -> void
    {
        alignas(16) uint8_t params[PARAMS]{};
        auto* at = value_in(parameter, params);
        write_real(xy[0], at, x);
        write_real(xy[1], at, y);
        call(object, function, params);
    }

    auto MarkerLayer::set_real(UObject* object, UFunction* function, FProperty* parameter, double value) -> void
    {
        alignas(16) uint8_t params[PARAMS]{};
        write_real(parameter, params, value);
        call(object, function, params);
    }

    auto MarkerLayer::collapse() -> void
    {
        m_markers.clear();
        if (m_widget.object && m_widget.alive()) set_root_shown(false);
    }

    auto MarkerLayer::hide() -> void
    {
        m_markers.clear();
        if (!m_widget.object) return;
        if (m_widget.alive())
        {
            alignas(16) uint8_t params[PARAMS]{};
            call(m_widget.object, m_remove, params);
        }
        forget();
    }

    auto MarkerLayer::forget() -> void
    {
        m_widget = m_canvas = {};
        m_shapes = {};
        m_label = {};
        m_label_cm = -1;
        m_root_shown = false;
        m_markers.clear();
    }
} // namespace dw::smoothwalker::ui
