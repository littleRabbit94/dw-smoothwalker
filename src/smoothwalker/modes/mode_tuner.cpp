// The camera position tuner (modes/mode_tuner.hpp). Bodies moved from the header unchanged; the helpers that need no
// tuner state are file-local.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "mode_tuner.hpp"
#include "../../common/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dw::smoothwalker::modes
{
    using namespace RC;
    using namespace RC::Unreal;

namespace
{
    auto bool_in(UStruct* owner, const wchar_t* name) -> FBoolProperty*
    {
        if (!owner) return nullptr;
        for (FProperty* property : owner->ForEachProperty())
        {
            if (property->GetName() == name) return CastField<FBoolProperty>(property);
        }
        return nullptr;
    }

    auto offset_in(UStruct* owner, const wchar_t* name) -> int32_t
    {
        if (!owner) return -1;
        for (FProperty* property : owner->ForEachProperty())
        {
            if (property->GetName() == name) return property->GetOffset_ForInternal();
        }
        return -1;
    }

    auto read_float(UObject* object, int32_t offset) -> float
    {
        float v{};
        memcpy(&v, reinterpret_cast<uint8_t*>(object) + offset, sizeof(v));
        return v;
    }

    auto write_float(UObject* object, int32_t offset, float value) -> void
    {
        memcpy(reinterpret_cast<uint8_t*>(object) + offset, &value, sizeof(value));
    }

    // Bitwise: every value the mod wrote went in verbatim, and a NaN must still equal itself.
    template <typename T>
    auto same(T a, T b) -> bool
    {
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }

    auto camera_type_label(uint8_t key) -> const wchar_t*
    {
        return key == 1 ? L"Default" : key == 2 ? L"Interior" : key == 3 ? L"Habitat" : L"?";
    }
} // namespace

    auto ModeTuner::capture() -> void
    {
        if (!m_layout_ok && !resolve_layout()) return;
        drop_dead_classes();
        for (auto& mode : m_modes)
        {
            if (mode.captured || !mode.usable || mode.missing) continue;
            auto path = std::wstring(L"/Game/_Dawnwalker/Player/Camera/Modes/") + mode.spec.folder + L"BP_CameraMode_" + mode.spec.name + L".Default__BP_CameraMode_" +
                        mode.spec.name + L"_C";
            auto* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
            if (!cdo)
            {
                // A failed lookup costs about 50 ms (measured 2026-09-20, CombatSprinting at night), so an
                // unloaded class is not asked for again until a map load or one of its modes is pushed.
                mode.missing = true;
                continue;
            }
            if (!mode.has_shipped)
            {
                read_values(cdo, mode.shipped);
                // A wrong layout would put every write in the wrong memory.
                if (!plausible(mode.shipped))
                {
                    Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {} reads as fov {}, {} offsets: not a camera layout, left as shipped\n"),
                                                    mode.spec.name, mode.shipped.fov, mode.shipped.offsets.size());
                    mode.usable = false;
                    continue;
                }
                mode.has_shipped = true;
                mode.base = mode.shipped;
                if (std::wstring_view(mode.spec.name) == L"Base_LongRange" && !mode.shipped.offsets.empty() &&
                    dw::verbose())
                {
                    auto& first = mode.shipped.offsets.front();
                    Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] Base_LongRange: fov {}, game lag {}/{}, pitch {}/{}, {} offsets, key {} at ({}, {}, {})\n"),
                                                   mode.shipped.fov, mode.shipped.hlag_on ? STR("on") : STR("off"),
                                                   mode.shipped.vlag_on ? STR("on") : STR("off"), mode.shipped.pitch_min,
                                                   mode.shipped.pitch_max, mode.shipped.offsets.size(), first.key, first.x, first.y, first.z);
                }
            }
            mode.cdo = cdo;
            mode.cls = cdo->GetClassPrivate();
            mode.cdo_ref = LiveRef::of(cdo);
            mode.cls_ref = LiveRef::of(mode.cls);
            mode.captured = true;
        }
    }

    auto ModeTuner::drop_dead_classes() -> void
    {
        for (auto& mode : m_modes)
        {
            if (!mode.captured || (mode.cdo_ref.alive() && mode.cls_ref.alive())) continue;
            mode.captured = false;
            mode.cdo = nullptr;
            mode.cls = nullptr;
            mode.cdo_ref = mode.cls_ref = {};
        }
    }

    auto ModeTuner::forget() -> void
    {
        for (auto& mode : m_modes)
        {
            mode.captured = false;
            mode.missing = false;
            mode.cdo = nullptr;
            mode.cls = nullptr;
            mode.cdo_ref = mode.cls_ref = {};
        }
        m_flip_stage = Idle;
        m_flip_again = false;
        m_flip_camera = nullptr;
        m_live.clear();
        m_untracked.clear();
        m_scan_needed = true;
        m_type_seen = m_type_from = 0;
    }

    auto ModeTuner::mode_state(UObject* player_camera, bool sprint) -> ModeState
    {
        ModeState state;
        if (m_scan_needed) ensure_scanned(player_camera);
        else adopt_new();
        uint8_t type = 0;
        if (m_get_type && player_camera && is_rebel_camera(player_camera))
        {
            // The flip switches the type away from its home for one update: that is not the game leaving or
            // entering an interior, so the type it left stands in for the read.
            if (m_flip_stage == Away && player_camera == m_flip_camera) type = m_flip_home;
            else type = get_camera_type(player_camera);
            state.interior = type == INTERIOR_KEY;
        }
        if (type != 0 && type != m_type_seen)
        {
            if (m_type_seen != 0)
            {
                m_type_from = m_type_seen;
                m_type_changed_at = m_seconds;
            }
            m_type_seen = type;
        }
        const Mode* exploring = nullptr;
        if (m_layout_ok && m_get_state && !m_scan_needed)
        {
            const Mode* far_mode = sprint ? mode_named(L"Base_LongRange") : nullptr;
            const Mode* close_mode = sprint ? mode_named(L"Base_CloseRange") : nullptr;
            uint64_t newest = 0;
            for_each_live([&](LiveMode& live, Mode& mode) {
                if (sprint && (&mode == far_mode || &mode == close_mode) && live.seq > newest)
                {
                    exploring = &mode;
                    newest = live.seq;
                }
                bool* found = mode.spec.group == Aiming           ? &state.aiming
                              : mode.spec.group == Combat         ? &state.combat
                              : mode.spec.group == Focus          ? &state.focus
                              : mode.spec.group == Traversal      ? &state.traversal
                              : mode.spec.group == Sprint && sprint ? &state.sprint
                                                                  : nullptr;
                if (!found || *found) return;
                *found = get_state(live.ref.object) <= 1;
            });
        }
        if (sprint && m_layout_ok) sprint_fovs(state, exploring, type);
        return state;
    }

    auto ModeTuner::list_modes(UObject* player_camera, bool& stale) -> std::vector<ModeListing>
    {
        stale = m_scan_needed;
        if (!m_layout_ok || !m_get_state || !player_camera) return {};
        adopt_new(); // mode_state() does this each tick while on; off, only an apply or this keeps the lists current
        std::vector<std::pair<uint64_t, ModeListing>> found;
        bool tuned = m_applied && m_last.active;
        drop_dead_classes();
        for (auto& live : m_live)
        {
            auto& mode = m_modes[live.index];
            if (!mode.captured || mode.cls != live.ref.cls || !live.ref.alive() || live.ref.object->GetOuterPrivate() != player_camera) continue;
            auto state = get_state(live.ref.object);
            if (state <= 2) found.push_back({live.seq, {mode.spec.name, state, mode.spec.group, tuned}});
        }
        std::erase_if(m_untracked, [](auto& entry) { return !entry.ref.alive(); });
        for (auto& entry : m_untracked)
        {
            if (entry.ref.object->GetOuterPrivate() != player_camera) continue;
            auto state = get_state(entry.ref.object);
            if (state <= 2) found.push_back({entry.seq, {entry.name, state, -1, false}});
        }
        std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::vector<ModeListing> out;
        out.reserve(found.size());
        for (auto& entry : found) out.push_back(std::move(entry.second));
        return out;
    }

    auto ModeTuner::camera_type_name(UObject* player_camera) -> std::wstring
    {
        if (!m_get_type || !player_camera) return {};
        if (!is_rebel_camera(player_camera)) return L"n/a";
        if (!m_type_names_read)
        {
            m_type_names_read = true;
            auto* ret = m_get_type->GetReturnProperty();
            UEnum* type_enum = nullptr;
            if (auto* as_enum = CastField<FEnumProperty>(ret)) type_enum = as_enum->GetEnum();
            else if (auto* as_byte = CastField<FByteProperty>(ret)) type_enum = as_byte->GetEnum();
            if (ret) m_type_offset = ret->GetOffset_ForInternal();
            if (type_enum)
            {
                std::vector<std::pair<FName, int64>> names;
                type_enum->GetEnumNamesAsVector(names);
                for (auto& [name, value] : names)
                {
                    auto text = name.ToString();
                    if (auto colons = text.rfind(L"::"); colons != std::wstring::npos) text.erase(0, colons + 2);
                    m_type_names.push_back({value, std::move(text)});
                }
            }
        }
        uint8_t params[16]{};
        if (m_type_offset < 0 || m_type_offset >= static_cast<int32_t>(sizeof(params))) return {};
        player_camera->ProcessEvent(m_get_type, params);
        int64 value = params[m_type_offset];
        for (auto& [known, name] : m_type_names)
        {
            if (known == value) return name;
        }
        return std::to_wstring(value);
    }

    auto ModeTuner::camera_changed() -> void
    {
        m_scan_needed = true;
        m_type_seen = m_type_from = 0;
    }

    auto ModeTuner::note_new(LiveRef object, bool game_thread) -> void
    {
        if (game_thread && m_layout_ok && m_applied)
        {
            drop_dead_classes();
            if (auto* mode = keep_if_mode(object, true))
            {
                write_new(*mode, object);
                return;
            }
        }
        std::lock_guard guard(m_new_mutex);
        if (m_new.size() < MAX_NEW) m_new.push_back(object);
        else m_new_overflow = true;
    }

    auto ModeTuner::apply(const PositionTuning& tuning, UObject* player_camera) -> void
    {
        auto started = std::chrono::steady_clock::now();
        ensure_scanned(player_camera);
        if (!m_layout_ok) return;
        adopt_foreign(); // before the writes below overwrite what another mod put there
        int classes = 0;
        for (auto& mode : m_modes)
        {
            if (!mode.captured) continue;
            write(mode.cdo, mode, tuning);
            ++classes;
        }
        int live = 0;
        for_each_live([&](LiveMode& entry, Mode& mode) {
            write(entry.ref.object, mode, tuning);
            entry.known = true;
            ++live;
        });
        m_applied = true;
        m_last = tuning;
        m_transition = static_cast<float>(tuning.transition);
        // Only CameraOffsets needs the flip; the lag switch and an inactive tuning move none.
        if (tuning.active || m_was_active) request_flip(player_camera);
        m_was_active = tuning.active;
        if (dw::verbose())
        {
            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] camera position applied: {} mode classes, {} live modes, {:.1f} ms\n"), classes, live, ms);
        }
    }

    auto ModeTuner::restore() -> void
    {
        if (!m_applied) return;
        capture(); // forget() may have dropped the CDO pointers since the last apply
        if (!m_layout_ok) return;
        adopt_foreign();
        if (m_flip_stage != Idle) set_blend(false); // unloaded mid-glide: the game's blend time back, a plain write
        PositionTuning neutral;
        neutral.active = false;
        neutral.own_lag = false;
        for (auto& mode : m_modes)
        {
            if (mode.captured) write(mode.cdo, mode, neutral);
        }
    }

    auto ModeTuner::tick(uint64_t view_updates, double view_seconds, UObject* player_camera) -> void
    {
        m_seconds = view_seconds;
        bool updated = view_updates != m_flip_seen;
        m_flip_seen = view_updates;
        if (m_flip_stage == Idle) return;
        if (!player_camera) return; // briefly unpossessed: wait, the camera usually comes back
        if (player_camera != m_flip_camera)
        {
            // A different pawn; its camera gets a fresh apply.
            set_blend(false);
            m_flip_stage = Idle;
            m_flip_again = false;
            m_flip_camera = nullptr;
            return;
        }
        if (!updated) return;

        switch (m_flip_stage)
        {
        case Pending: {
            auto type = get_camera_type(m_flip_camera);
            if (type < 1 || type > 3)
            {
                set_blend(false); // a flip restarted from Back still holds the transition blend time
                m_flip_stage = Idle;
                return;
            }
            set_blend(true);
            m_flip_home = type;
            m_flip_away = type == 1 ? 2 : 1;
            set_camera_type(m_flip_camera, m_flip_away);
            m_flip_stage = Away;
            return;
        }
        case Away:
            // If the game changed the type itself meanwhile (entering an interior), its choice stands.
            if (get_camera_type(m_flip_camera) == m_flip_away) set_camera_type(m_flip_camera, m_flip_home);
            m_flip_back_at = view_seconds;
            m_flip_stage = Back;
            return;
        case Back: {
            // The running blend keeps the settings it started with.
            if (view_seconds - m_flip_back_at < m_transition + 0.25) return;
            set_blend(false);
            m_flip_stage = m_flip_again ? Pending : Idle;
            m_flip_again = false;
            return;
        }
        default:
            return;
        }
    }

    auto ModeTuner::resolve_layout() -> bool
    {
        if (m_layout_tried) return false;
        m_layout_tried = true;
        auto* mode = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraMode"));
        auto* tpp = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraModeTPP"));
        auto* offset = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/RebelCamera.CameraOffset"));
        auto* blend = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/Engine.AlphaBlendArgs"));
        m_set_type = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraComponent:SetCameraType"));
        m_get_type = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraComponent:GetCameraType"));

        m_get_state = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraMode:GetState"));
        m_mode_base = mode;

        m_off.fov = offset_in(mode, STR("DefaultFieldOfView"));
        m_off.pitch_min = offset_in(mode, STR("ViewPitchMin"));
        m_off.pitch_max = offset_in(mode, STR("ViewPitchMax"));
        m_hlag_on = bool_in(mode, STR("bEnableCameraHorizontalLag"));
        m_vlag_on = bool_in(mode, STR("bEnableCameraVerticalLag"));
        if (!m_hlag_on) m_hlag_on = bool_in(tpp, STR("bEnableCameraHorizontalLag"));
        if (!m_vlag_on) m_vlag_on = bool_in(tpp, STR("bEnableCameraVerticalLag"));
        m_off.offsets_map = offset_in(tpp, STR("CameraOffsets"));
        auto type_blend_args = offset_in(tpp, STR("CameraTypeBlendArgs"));
        auto blend_time = offset_in(blend, STR("BlendTime"));
        m_off.type_blend = (type_blend_args >= 0 && blend_time >= 0) ? type_blend_args + blend_time : -1;
        m_off.offset_target = offset_in(offset, STR("TargetOffset"));
        m_off.offset_fov = offset_in(offset, STR("OverriddenFieldOfView"));
        m_offset_override = bool_in(offset, STR("bOverrideFOV"));
        if (offset)
        {
            m_off.offset_size = offset->GetStructureSize();
            m_off.offset_align = offset->GetMinAlignment();
        }

        bool ok = m_off.fov >= 0 && m_off.pitch_min >= 0 && m_off.pitch_max >= 0 && m_hlag_on && m_vlag_on &&
                  m_off.offsets_map >= 0 && m_off.type_blend >= 0 && m_off.offset_target >= 0 && m_off.offset_fov >= 0 && m_offset_override &&
                  m_off.offset_size > 0 && m_off.offset_align > 0 && m_set_type && m_get_type;
        if (!ok)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] camera mode layout not found, camera position tuning off\n"));
            return false;
        }
        m_map_layout = FScriptMap::GetScriptLayout(1, 1, m_off.offset_size, m_off.offset_align); // ECameraType key: one byte
        m_layout_ok = true;
        if (dw::verbose())
        {
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] camera mode layout: offsets map 0x{:X}, CameraOffset {} bytes, value at 0x{:X}\n"),
                                           m_off.offsets_map, m_off.offset_size, m_map_layout.ValueOffset);
        }
        return true;
    }

    auto ModeTuner::plausible(const Original& o, bool foreign) -> bool
    {
        if (!(o.fov >= 30.0f && o.fov <= 170.0f)) return false;
        if (!(o.pitch_min >= -90.0f && o.pitch_min <= 0.0f && o.pitch_max >= 0.0f && o.pitch_max <= 90.0f)) return false;
        if (o.offsets.empty() || o.offsets.size() > 3) return false;
        for (auto& off : o.offsets)
        {
            if (off.key < 1 || off.key > 3) return false;
            if (!(std::abs(off.x) <= 2000.0 && std::abs(off.y) <= 2000.0 && std::abs(off.z) <= 2000.0)) return false;
            // Shipped values are unused while the override is off (160 on most modes, 0 or 90 on some); the ceiling
            // is DefaultFieldOfView's. Also rejects NaN.
            if (foreign && !(off.overridden_fov >= 0.0f && off.overridden_fov <= 170.0f)) return false;
        }
        return true;
    }

    auto ModeTuner::keep_if_mode(LiveRef ref, bool known) -> Mode*
    {
        for (size_t i = 0; i < m_modes.size(); ++i)
        {
            if (!m_modes[i].captured || m_modes[i].cls != ref.cls || ref.object == m_modes[i].cdo) continue;
            auto held = std::find_if(m_live.begin(), m_live.end(), [&](auto& entry) { return entry.ref.object == ref.object; });
            if (held == m_live.end()) m_live.push_back({ref, i, ++m_seq, known});
            else held->known = held->known || known;
            return &m_modes[i];
        }
        return nullptr;
    }

    auto ModeTuner::keep_untracked(LiveRef ref) -> void
    {
        if (!m_mode_base || !ref.cls || !ref.cls->IsChildOf(m_mode_base)) return;
        if (std::any_of(m_untracked.begin(), m_untracked.end(), [&](auto& entry) { return entry.ref.object == ref.object; })) return;
        if (m_untracked.size() >= MAX_UNTRACKED) std::erase_if(m_untracked, [](auto& entry) { return !entry.ref.alive(); });
        if (m_untracked.size() >= MAX_UNTRACKED) m_untracked.erase(m_untracked.begin());
        auto name = ref.cls->GetName();
        if (name.starts_with(L"BP_CameraMode_")) name.erase(0, 14);
        if (name.ends_with(L"_C")) name.resize(name.size() - 2);
        m_untracked.push_back({ref, std::move(name), ++m_seq});
    }

    auto ModeTuner::scan_instances(UObject* player_camera) -> void
    {
        drop_dead_classes();
        m_live.clear();
        m_untracked.clear();
        if (!player_camera) return;
        UObjectGlobals::ForEachUObject([&](UObject* object, int32, int32) {
            if (object->GetOuterPrivate() != player_camera) return LoopAction::Continue;
            auto ref = LiveRef::of(object);
            if (!keep_if_mode(ref, false)) keep_untracked(ref);
            return LoopAction::Continue;
        });
        m_scan_needed = false;
    }

    auto ModeTuner::ensure_scanned(UObject* player_camera) -> void
    {
        capture();
        if (!m_layout_ok) return;
        adopt_new();
        if (m_scan_needed) scan_instances(player_camera);
    }

    auto ModeTuner::adopt_new() -> void
    {
        std::vector<LiveRef> fresh;
        {
            std::lock_guard guard(m_new_mutex);
            fresh.swap(m_new);
            if (m_new_overflow) m_scan_needed = true;
            m_new_overflow = false;
        }
        drop_dead_classes();
        std::erase_if(m_live, [](auto& entry) { return !entry.ref.alive(); });
        for (auto& ref : fresh)
        {
            if (!ref.alive()) continue;
            if (auto* mode = keep_if_mode(ref, true)) write_new(*mode, ref);
            else if (!adopt_late(ref)) keep_untracked(ref);
        }
    }

    auto ModeTuner::write_new(Mode& mode, LiveRef ref) -> void
    {
        if (!m_applied) return;
        std::vector<Original> seen(2);
        read_values(mode.cdo, seen[0]);
        read_values(ref.object, seen[1]);
        adopt_from(mode, seen);
        write(mode.cdo, mode, m_last);
        write(ref.object, mode, m_last);
    }

    auto ModeTuner::adopt_late(LiveRef ref) -> bool
    {
        if (!m_applied) return false;
        auto name = ref.cls->GetName();
        for (auto& mode : m_modes)
        {
            if (mode.captured || !mode.usable) continue;
            if (name != std::wstring(L"BP_CameraMode_") + mode.spec.name + L"_C") continue;
            mode.missing = false;
            capture();
            if (dw::verbose())
                Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] {} loaded late: {}\n"), mode.spec.name, mode.captured ? STR("tuned") : STR("not found"));
            if (!mode.captured || !keep_if_mode(ref, true)) return false;
            std::vector<Original> seen(2);
            read_values(mode.cdo, seen[0]);
            read_values(ref.object, seen[1]);
            adopt_from(mode, seen);
            write(mode.cdo, mode, m_last);
            write(ref.object, mode, m_last);
            if (m_last.active) request_flip(ref.object->GetOuterPrivate());
            return true;
        }
        return false;
    }

    template <typename Visit>
    auto ModeTuner::for_each_live(Visit&& visit) -> void
    {
        drop_dead_classes();
        for (auto& live : m_live)
        {
            auto& mode = m_modes[live.index];
            if (!mode.captured || mode.cls != live.ref.cls || !live.ref.alive()) continue;
            visit(live, mode);
        }
    }

    template <typename Visit>
    auto ModeTuner::for_each_instance(Visit&& visit) -> void
    {
        for_each_live([&](LiveMode& live, Mode& mode) { visit(live.ref.object, mode); });
    }

    auto ModeTuner::set_blend(bool transition) -> void
    {
        for_each_instance([&](UObject* instance, Mode& mode) {
            write_float(instance, m_off.type_blend, transition ? m_transition : mode.base.type_blend);
        });
    }

    template <typename Visit>
    auto ModeTuner::for_each_offset(UObject* object, Visit&& visit) -> void
    {
        auto* map = reinterpret_cast<FScriptMap*>(reinterpret_cast<uint8_t*>(object) + m_off.offsets_map);
        int32 max = map->GetMaxIndex();
        for (int32 i = 0; i < max; ++i)
        {
            if (!map->IsValidIndex(i)) continue;
            auto* pair = static_cast<uint8_t*>(map->GetData(i, m_map_layout));
            visit(pair[0], pair + m_map_layout.ValueOffset);
        }
    }

    auto ModeTuner::read_values(UObject* object, Original& out) -> void
    {
        out.fov = read_float(object, m_off.fov);
        out.pitch_min = read_float(object, m_off.pitch_min);
        out.pitch_max = read_float(object, m_off.pitch_max);
        out.hlag_on = m_hlag_on->GetPropertyValueInContainer(object);
        out.vlag_on = m_vlag_on->GetPropertyValueInContainer(object);
        out.type_blend = read_float(object, m_off.type_blend);
        out.offsets.clear();
        for_each_offset(object, [&](uint8_t key, uint8_t* value) {
            OffsetValues o{key};
            double target[3]{};
            memcpy(target, value + m_off.offset_target, sizeof(target));
            o.x = target[0];
            o.y = target[1];
            o.z = target[2];
            memcpy(&o.overridden_fov, value + m_off.offset_fov, sizeof(float));
            o.override_fov = m_offset_override->GetPropertyValueInContainer(value);
            out.offsets.push_back(o);
        });
    }

    auto ModeTuner::expected(const Mode& mode, const PositionTuning& t) -> Original
    {
        const auto& b = mode.base;
        const auto& g = t.groups[mode.spec.group];
        bool on = t.active;
        Original e = b;

        e.fov = on ? static_cast<float>(b.fov + g.fov) : b.fov;
        e.hlag_on = b.hlag_on && !t.own_lag;
        e.vlag_on = b.vlag_on && !t.own_lag;
        // Aiming and combat ship wider limits; only the modes listed as player_pitch take the setting. The
        // setting is absolute, so it stays on top of limits another mod wrote into such a mode.
        bool player_pitch = mode.spec.player_pitch;
        e.pitch_min = on && player_pitch ? static_cast<float>(t.pitch_min) : b.pitch_min;
        e.pitch_max = on && player_pitch ? static_cast<float>(t.pitch_max) : b.pitch_max;

        if (!on) return e;
        // The group's tuning goes on every key; offset_of() adds the interior settings on key 2 of the modes that take them.
        for (auto& off : e.offsets) // each entry from its own base value, so in place
        {
            off = offset_of(off, e.fov, g, t.interior, mode.spec.interior, t.shoulder_swap);
        }
        return e;
    }

    auto ModeTuner::write(UObject* object, const Mode& mode, const PositionTuning& t) -> void
    {
        auto e = expected(mode, t);
        write_float(object, m_off.fov, e.fov);
        // The game reads these every frame (tested live, 2026-09-19): off, the follow is the only lag.
        m_hlag_on->SetPropertyValueInContainer(object, e.hlag_on);
        m_vlag_on->SetPropertyValueInContainer(object, e.vlag_on);
        write_float(object, m_off.pitch_min, e.pitch_min);
        write_float(object, m_off.pitch_max, e.pitch_max);

        for_each_offset(object, [&](uint8_t key, uint8_t* value) {
            for (auto& off : e.offsets)
            {
                if (off.key != key) continue;
                double target[3]{off.x, off.y, off.z};
                memcpy(value + m_off.offset_target, target, sizeof(target));
                memcpy(value + m_off.offset_fov, &off.overridden_fov, sizeof(float));
                m_offset_override->SetPropertyValueInContainer(value, off.override_fov);
                break;
            }
        });
    }

    auto ModeTuner::same_keys(const Original& a, const Original& b) -> bool
    {
        if (a.offsets.size() != b.offsets.size()) return false;
        return std::all_of(a.offsets.begin(), a.offsets.end(), [&](auto& off) {
            return std::any_of(b.offsets.begin(), b.offsets.end(), [&](auto& other) { return other.key == off.key; });
        });
    }

    auto ModeTuner::adopt_foreign() -> void
    {
        drop_dead_classes();
        std::vector<std::vector<Original>> seen(m_modes.size());
        for (size_t i = 0; i < m_modes.size(); ++i)
        {
            if (m_modes[i].captured) read_values(m_modes[i].cdo, seen[i].emplace_back());
        }
        for_each_live([&](LiveMode& live, Mode& mode) {
            if (live.known) read_values(live.ref.object, seen[static_cast<size_t>(&mode - m_modes.data())].emplace_back());
        });
        for (size_t i = 0; i < m_modes.size(); ++i)
        {
            if (!seen[i].empty()) adopt_from(m_modes[i], seen[i]);
        }
    }

    auto ModeTuner::adopt_from(Mode& mode, const std::vector<Original>& seen) -> void
    {
        for (auto& values : seen)
        {
            if (same_keys(values, mode.base)) continue;
            if (!mode.keys_warned)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {}: CameraOffsets holds {} camera types, not the {} captured: values from other mods not taken\n"),
                                                mode.spec.name, values.offsets.size(), mode.base.offsets.size());
            }
            mode.keys_warned = true;
            return;
        }
        // Classified against the base as it stood before this check: an instance still holding the last write
        // made from it is the mod's own even after the CDO's foreign value changed the base.
        const Original prior = mode.base;
        const Original written = m_applied ? expected(mode, m_last) : prior;
        // field: 0-2 the scalars, 3 + 5 * key index + axis the offsets (x, y, z, FOV override value, FOV override
        // switch); the out-of-range warning once per field per check, not once per object holding the value. get: one field of an Original. label: that field
        // with a value, for the log.
        uint32_t warned = 0;
        auto consider = [&](int field, auto value, auto get, auto label) {
            if (same(value, get(prior)) || same(value, get(written)) || same(value, get(mode.shipped))) return;
            if (same(value, get(mode.base))) return; // taken from an earlier object in this check
            auto candidate = mode.base;
            get(candidate) = value;
            if (!plausible(candidate, true))
            {
                if (!(warned & (1u << field)))
                {
                    Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {}: {} written by another mod is out of range, base left at {}\n"),
                                                    mode.spec.name, label(value), get(mode.base));
                }
                warned |= 1u << field;
                return;
            }
            mode.base = std::move(candidate);
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] {}: {} written by another mod (shipped {}), used as its base\n"), mode.spec.name,
                                           label(value), get(mode.shipped));
        };
        auto scalar = [](const wchar_t* name) { return [name](auto v) { return std::format(L"{} {}", name, v); }; };
        for (auto& values : seen)
        {
            consider(0, values.fov, [](auto& o) -> auto& { return o.fov; }, scalar(L"FOV"));
            consider(1, values.pitch_min, [](auto& o) -> auto& { return o.pitch_min; }, scalar(L"pitch min"));
            consider(2, values.pitch_max, [](auto& o) -> auto& { return o.pitch_max; }, scalar(L"pitch max"));
            // Base, shipped and the last write share one key order (expected() copies the base, the base copies
            // shipped); the object's own map order may differ, so it is matched by key.
            for (size_t k = 0; k < mode.base.offsets.size(); ++k)
            {
                uint8_t key = mode.base.offsets[k].key;
                auto found = std::find_if(values.offsets.begin(), values.offsets.end(), [&](auto& off) { return off.key == key; });
                if (found == values.offsets.end()) continue; // same_keys() rules this out
                auto offset = [key](const wchar_t* name) {
                    return [name, key](auto v) { return std::format(L"{} {} on key {} ({})", name, v, key, camera_type_label(key)); };
                };
                int field = 3 + 5 * static_cast<int>(k); // plausible() allows at most 3 keys: bits 3-17
                consider(field, found->x, [k](auto& o) -> auto& { return o.offsets[k].x; }, offset(L"X offset"));
                consider(field + 1, found->y, [k](auto& o) -> auto& { return o.offsets[k].y; }, offset(L"Y offset"));
                consider(field + 2, found->z, [k](auto& o) -> auto& { return o.offsets[k].z; }, offset(L"Z offset"));
                consider(field + 3, found->overridden_fov, [k](auto& o) -> auto& { return o.offsets[k].overridden_fov; }, offset(L"FOV override"));
                consider(field + 4, found->override_fov, [k](auto& o) -> auto& { return o.offsets[k].override_fov; }, offset(L"FOV override switch"));
            }
        }
    }

    auto ModeTuner::get_state(UObject* mode) -> uint8_t
    {
        uint8_t params[16]{};
        params[0] = 0xFF;
        mode->ProcessEvent(m_get_state, params);
        return params[0];
    }

    auto ModeTuner::get_camera_type(UObject* camera) -> uint8_t
    {
        uint8_t params[16]{};
        camera->ProcessEvent(m_get_type, params);
        return params[m_type_offset >= 0 && m_type_offset < static_cast<int32_t>(sizeof(params)) ? m_type_offset : 0];
    }

    auto ModeTuner::is_rebel_camera(UObject* camera) -> bool
    {
        if (!m_camera_class_read)
        {
            m_camera_class_read = true;
            m_camera_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraComponent"));
        }
        auto* cls = camera->GetClassPrivate();
        return m_camera_class && cls && cls->IsChildOf(m_camera_class);
    }

    auto ModeTuner::set_camera_type(UObject* camera, uint8_t type) -> void
    {
        uint8_t params[16]{};
        params[0] = type;
        camera->ProcessEvent(m_set_type, params);
    }

    auto ModeTuner::mode_named(const wchar_t* name) const -> const Mode*
    {
        for (auto& mode : m_modes)
        {
            if (std::wcscmp(mode.spec.name, name) == 0) return &mode;
        }
        return nullptr;
    }

    auto ModeTuner::sprint_fovs(ModeState& state, const Mode* exploring, uint8_t type) const -> void
    {
        if (!exploring) exploring = mode_named(L"Base_LongRange");
        const Mode* sprint = mode_named(L"Sprint");
        auto usable = [](const Mode* mode) { return mode && mode->has_shipped && mode->usable; };
        if (!usable(exploring) || !usable(sprint)) return;
        // As the last apply wrote them (m_last; neutral before the first, which writes the base as it stands).
        auto fov = [&](const Mode* mode, uint8_t on) { return shown_fov(mode->base.fov, mode->base.offsets, mode->spec, m_last, on ? on : 1); };
        auto moves = [&](uint8_t a, uint8_t b) { return a != b && (fov(exploring, a) != fov(exploring, b) || fov(sprint, a) != fov(sprint, b)); };
        state.exploring_fov = fov(exploring, type);
        state.sprint_fov = fov(sprint, type);
        // The game's blend runs CameraTypeBlendArgs (1.0 s shipped) on world time; the flip's while it is away or
        // gliding back (the blend time then is position_transition, and Back holds 0.25 s past it).
        const double blend = std::clamp(static_cast<double>(exploring->base.type_blend), 0.0, 5.0) + 0.1;
        const bool game_blend = m_type_from != 0 && m_seconds - m_type_changed_at < blend && moves(m_type_from, m_type_seen);
        const bool flip_blend = (m_flip_stage == Away || m_flip_stage == Back) && moves(m_flip_home, m_flip_away);
        state.fov_moves = game_blend || flip_blend;
    }

    auto ModeTuner::request_flip(UObject* camera) -> void
    {
        if (!camera) return;
        if (m_flip_stage == Idle || (m_flip_stage == Back && camera == m_flip_camera))
        {
            m_flip_camera = camera;
            m_flip_stage = Pending;
            m_flip_again = false;
        }
        else if (m_flip_stage != Pending) m_flip_again = true;
    }
} // namespace dw::smoothwalker::modes
