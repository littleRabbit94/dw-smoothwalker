// Camera position tuning: per-group distance, height, shoulder and FOV, and the look limits, plus the
// switch for the game's own camera lag, written into the game's camera modes. Game thread only, except note_new() (any thread) and
// restore() at unload.
//
// Every write is computed from the CDO values captured the first time a class is seen, never from the
// current value, so applies cannot compound. CameraOffsets is only re-read on a camera type change, so an
// apply flips the player's camera type away and back (docs/design.md, "How writes land").
#pragma once

#include "config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dwsc
{
    using namespace RC;
    using namespace RC::Unreal;

    // A UObject pointer kept across ticks, with the object array index it had when taken from a live object.
    // alive() reads only GUObjectArray, never the object: true while the slot still holds the same pointer and
    // is neither Unreachable (1 << 28) nor Garbage (1 << 21, UE 5.5.4 ObjectMacros.h), catching a GC even with no
    // EndPlay or LoadMap hook. UE4SS's FUObjectItem::IsPendingKill tests bit 29, which UE 5 reuses, so it is not
    // used. The class is compared too: a freed object replaced by another class at the same address and index
    // must not pass, or a cached property offset reads the wrong layout. Game thread only.
    struct LiveRef
    {
        UObject* object = nullptr;
        int32_t index = -1;
        UClass* cls = nullptr;

        static auto of(UObject* live) -> LiveRef
        {
            return live ? LiveRef{live, live->GetInternalIndex(), live->GetClassPrivate()} : LiveRef{};
        }

        auto alive() const -> bool
        {
            if (!object || index < 0) return false;
            static constexpr auto DEAD = static_cast<EInternalObjectFlags>((1 << 28) | (1 << 21));
            auto* item = FUObjectArray::IndexToObject(index);
            return item && item->GetUObject() == object && !item->HasAnyFlags(DEAD) && object->GetClassPrivate() == cls;
        }
    };

    enum Group : int
    {
        Exploration,
        Sprint,
        Combat,
        Focus,
        Aiming,
        Traversal,
        GroupCount
    };

    struct ModeClassSpec
    {
        Group group;
        const wchar_t* name;         // BP_CameraMode_<name>
        const wchar_t* folder = L""; // under Modes/, with its slash
    };

    // Finisher and shadowstep attack cameras are scripted shots and stay as shipped.
    inline const std::vector<ModeClassSpec>& mode_classes()
    {
        static const std::vector<ModeClassSpec> specs{
                {Exploration, L"Base"}, {Exploration, L"Base_LongRange"}, {Exploration, L"Base_CloseRange"},
                {Exploration, L"Base_CloseRange_Mantle2m"}, {Exploration, L"GapSqueeze"},
                {Sprint, L"Sprint"}, {Sprint, L"Sprint_VampiricFastTraversal"},
                {Combat, L"CombatNear"}, {Combat, L"CombatFromArm"}, {Combat, L"CombatFromArm_LongRange"},
                {Combat, L"CombatFromArm_VeryLongRange"}, {Combat, L"CombatFistFightMode"}, {Focus, L"FocusMode"},
                {Combat, L"CombatSprinting"},
                {Aiming, L"Aiming"}, {Aiming, L"AimingOnLadder"}, {Aiming, L"AimingClawRide"}, {Aiming, L"AimingClawRideLedge"},
                {Aiming, L"AntiGravAiming"},
                {Traversal, L"AntiGrav"}, {Traversal, L"ClawRide"}, {Traversal, L"ClawRideLedge"},
                {Traversal, L"Shadowstep_2_Base", L"Shadowstep/"},
        };
        return specs;
    }

    struct GroupTuning
    {
        double distance = 100; // percent
        double height = 0;     // cm
        double shoulder = 0;   // cm outward on the side the mode already sits
        double fov = 0;        // degrees added
    };

    struct PositionTuning
    {
        bool active = true;   // camera_tuning and enabled
        bool own_lag = true;  // enabled: the game's camera lag is off, so the follow is the only lag
        GroupTuning groups[GroupCount];
        bool shoulder_swap = false;
        double pitch_min = -60;
        double pitch_max = 40;
        double transition = 0.5; // s
    };

    inline auto operator==(const GroupTuning& a, const GroupTuning& b) -> bool
    {
        return a.distance == b.distance && a.height == b.height && a.shoulder == b.shoulder && a.fov == b.fov;
    }

    inline auto operator==(const PositionTuning& a, const PositionTuning& b) -> bool
    {
        for (int i = 0; i < GroupCount; ++i)
        {
            if (!(a.groups[i] == b.groups[i])) return false;
        }
        return a.active == b.active && a.own_lag == b.own_lag && a.shoulder_swap == b.shoulder_swap &&
               a.pitch_min == b.pitch_min && a.pitch_max == b.pitch_max && a.transition == b.transition;
    }

    inline auto position_of(const Settings& s) -> PositionTuning
    {
        PositionTuning p;
        p.active = s.camera_tuning && s.enabled;
        p.own_lag = s.enabled;
        p.groups[Exploration] = {s.exploration_distance, s.exploration_height, s.exploration_shoulder, s.exploration_fov};
        p.groups[Sprint] = {s.sprint_distance, s.sprint_height, s.sprint_shoulder, s.sprint_fov};
        p.groups[Combat] = {s.combat_distance, s.combat_height, s.combat_shoulder, s.combat_fov};
        p.groups[Focus] = {s.focus_distance, s.focus_height, s.focus_shoulder, s.focus_fov};
        p.groups[Aiming] = {s.aiming_distance, s.aiming_height, s.aiming_shoulder, s.aiming_fov};
        p.groups[Traversal] = {s.traversal_distance, s.traversal_height, 0.0, s.traversal_fov};
        p.shoulder_swap = s.shoulder_swap;
        p.pitch_min = s.pitch_min;
        p.pitch_max = s.pitch_max;
        p.transition = s.position_transition;
        return p;
    }

    // Whether an Aiming-, a Combat- or a Traversal-group mode is blending in or active, from one scan of the
    // player's live modes.
    struct ModeState
    {
        bool aiming = false;
        bool combat = false;
        bool traversal = false;
    };

    class ModeTuner
    {
      public:
        // Originals are kept for the session: a class that stays loaded across a map load still holds the
        // mod's values and must not have them captured as the game's.
        auto capture() -> void
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
                if (!mode.has_original)
                {
                    read_original(cdo, mode.original);
                    // A wrong layout would put every write in the wrong memory.
                    if (!plausible(mode.original))
                    {
                        Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {} reads as fov {}, {} offsets: not a camera layout, left as shipped\n"),
                                                        mode.spec.name, mode.original.fov, mode.original.offsets.size());
                        mode.usable = false;
                        continue;
                    }
                    mode.has_original = true;
                    if (std::wstring_view(mode.spec.name) == L"Base_LongRange" && !mode.original.offsets.empty())
                    {
                        auto& first = mode.original.offsets.front();
                        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] Base_LongRange: fov {}, game lag {}/{}, pitch {}/{}, {} offsets, key {} at ({}, {}, {})\n"),
                                                       mode.original.fov, mode.original.hlag_on ? STR("on") : STR("off"),
                                                       mode.original.vlag_on ? STR("on") : STR("off"), mode.original.pitch_min,
                                                       mode.original.pitch_max, mode.original.offsets.size(), first.key, first.x, first.y, first.z);
                    }
                }
                mode.cdo = cdo;
                mode.cls = cdo->GetClassPrivate();
                mode.cdo_ref = LiveRef::of(cdo);
                mode.cls_ref = LiveRef::of(mode.cls);
                mode.captured = true;
            }
        }

        // Without a LoadMap hook, forget() may not run before a class unloads; every use of a captured CDO or
        // class pointer is preceded by this check: capture(), scan_instances(), for_each_instance() (hence set_blend()).
        auto drop_dead_classes() -> void
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

        // Before a map load: classes may unload.
        auto forget() -> void
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
            m_scan_needed = true;
        }

        // Whether an Aiming-, Combat- or Traversal-group mode is blending in or active (ECameraModeState 0 or 1;
        // 2 blending out, 3 popped). One GetState call per live instance of those three groups per engine tick,
        // skipped once a group's flag is already set. False until an apply has scanned this camera.
        auto mode_state() -> ModeState
        {
            ModeState state;
            if (!m_layout_ok || !m_get_state || m_scan_needed) return state;
            adopt_new();
            for_each_instance([&](UObject* instance, Mode& mode) {
                bool* found = mode.spec.group == Aiming      ? &state.aiming
                              : mode.spec.group == Combat    ? &state.combat
                              : mode.spec.group == Traversal ? &state.traversal
                                                             : nullptr;
                if (!found || *found) return;
                uint8_t params[16]{};
                params[0] = 0xFF;
                instance->ProcessEvent(m_get_state, params);
                *found = params[0] <= 1;
            });
            return state;
        }

        // A new player camera: its modes were made before the new-object callback could see them.
        auto camera_changed() -> void
        {
            m_scan_needed = true;
        }

        // From the new-object callback, on whatever thread constructs the object, for an object whose outer is
        // the player's camera: only the hand-off. adopt_new() sorts modes from the rest on the game thread.
        auto note_new(LiveRef object) -> void
        {
            std::lock_guard guard(m_new_mutex);
            if (m_new.size() < MAX_NEW) m_new.push_back(object);
            else m_new_overflow = true;
        }

        auto apply(const PositionTuning& tuning, UObject* player_camera) -> void
        {
            auto started = std::chrono::steady_clock::now();
            capture();
            if (!m_layout_ok) return;
            int classes = 0;
            for (auto& mode : m_modes)
            {
                if (!mode.captured) continue;
                write(mode.cdo, mode, tuning);
                ++classes;
            }
            int live = 0;
            adopt_new();
            if (m_scan_needed) scan_instances(player_camera);
            for_each_instance([&](UObject* instance, Mode& mode) {
                write(instance, mode, tuning);
                ++live;
            });
            m_applied = true;
            m_last = tuning;
            m_transition = static_cast<float>(tuning.transition);
            // Only CameraOffsets needs the flip; the lag switch and an inactive tuning move none.
            if (tuning.active || m_was_active) request_flip(player_camera);
            m_was_active = tuning.active;
            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] camera position applied: {} mode classes, {} live modes, {:.1f} ms\n"), classes, live, ms);
        }

        // At unload, after the mod's callbacks are gone. Live instances keep their values until the game pushes
        // new ones.
        auto restore() -> void
        {
            if (!m_applied) return;
            capture(); // forget() may have dropped the CDO pointers since the last apply
            if (!m_layout_ok) return;
            if (m_flip_stage != Idle) set_blend(false); // unloaded mid-glide: the game's blend time back, a plain write
            PositionTuning neutral;
            neutral.active = false;
            neutral.own_lag = false;
            for (auto& mode : m_modes)
            {
                if (mode.captured) write(mode.cdo, mode, neutral);
            }
        }

        // Once per engine tick. A stage advances only after the player's camera has updated, so a flip
        // requested under a pause (the Mod Menu) waits for the world instead of running unseen.
        // view_seconds: world time of the player's camera updates. The glide runs on world time, so the wait for
        // its end must too, or a pause or slow motion mid-glide restores the blend time while it still blends.
        auto tick(uint64_t view_updates, double view_seconds, UObject* player_camera) -> void
        {
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

      private:
        struct OffsetOriginal
        {
            uint8_t key;
            double x, y, z;
            float overridden_fov;
        };

        struct Original
        {
            float fov{}, pitch_min{}, pitch_max{}, type_blend{};
            bool hlag_on{}, vlag_on{};
            std::vector<OffsetOriginal> offsets;
        };

        struct Mode
        {
            ModeClassSpec spec;
            UObject* cdo = nullptr;
            UClass* cls = nullptr;
            LiveRef cdo_ref, cls_ref; // checked before cdo or cls is used
            bool captured = false;
            bool missing = false; // not loaded at the last lookup
            bool has_original = false;
            bool usable = true;
            Original original;
        };

        struct Offsets
        {
            int32_t fov = -1, pitch_min = -1, pitch_max = -1;
            int32_t offsets_map = -1, type_blend = -1; // type_blend: CameraTypeBlendArgs.BlendTime
            int32_t offset_target = -1, offset_fov = -1;
            int32_t offset_size = 0, offset_align = 0;
        };

        std::vector<Mode> m_modes = [] {
            std::vector<Mode> modes;
            for (auto& spec : mode_classes()) modes.push_back({spec});
            return modes;
        }();
        std::vector<std::pair<LiveRef, size_t>> m_live; // the player's live modes, with their m_modes index
        PositionTuning m_last;                          // what the last apply wrote, for a class that loads later
        bool m_scan_needed = true;                      // m_live does not cover this camera yet
        static constexpr size_t MAX_NEW = 256;
        std::mutex m_new_mutex;                         // m_new, m_new_overflow: written by note_new on any thread
        std::vector<LiveRef> m_new;
        bool m_new_overflow = false;
        Offsets m_off;
        // bEnableCameraHorizontalLag / VerticalLag: bitfields sharing a byte, written through their masks.
        // Properties of a native class, so the pointers hold for the session.
        FBoolProperty* m_hlag_on = nullptr;
        FBoolProperty* m_vlag_on = nullptr;
        bool m_layout_ok = false;
        bool m_layout_tried = false;
        bool m_applied = false;
        bool m_was_active = false;
        FScriptMapLayout m_map_layout{};

        UFunction* m_set_type = nullptr;
        UFunction* m_get_type = nullptr;
        UFunction* m_get_state = nullptr; // RebelCameraMode:GetState; without it aiming() stays false

        enum FlipStage
        {
            Idle,
            Pending, // waiting for the camera to update
            Away,    // switched to another type
            Back     // switched back; waiting for the blend to end
        };
        FlipStage m_flip_stage = Idle;
        bool m_flip_again = false;
        UObject* m_flip_camera = nullptr;
        uint8_t m_flip_home = 0, m_flip_away = 0;
        uint64_t m_flip_seen = 0;
        float m_transition = 0.5f;
        double m_flip_back_at = 0.0; // view_seconds at the switch back

        static auto bool_in(UStruct* owner, const wchar_t* name) -> FBoolProperty*
        {
            if (!owner) return nullptr;
            for (FProperty* property : owner->ForEachProperty())
            {
                if (property->GetName() == name) return CastField<FBoolProperty>(property);
            }
            return nullptr;
        }

        static auto offset_in(UStruct* owner, const wchar_t* name) -> int32_t
        {
            if (!owner) return -1;
            for (FProperty* property : owner->ForEachProperty())
            {
                if (property->GetName() == name) return property->GetOffset_ForInternal();
            }
            return -1;
        }

        auto resolve_layout() -> bool
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
            if (offset)
            {
                m_off.offset_size = offset->GetStructureSize();
                m_off.offset_align = offset->GetMinAlignment();
            }

            bool ok = m_off.fov >= 0 && m_off.pitch_min >= 0 && m_off.pitch_max >= 0 && m_hlag_on && m_vlag_on &&
                      m_off.offsets_map >= 0 && m_off.type_blend >= 0 && m_off.offset_target >= 0 && m_off.offset_fov >= 0 &&
                      m_off.offset_size > 0 && m_off.offset_align > 0 && m_set_type && m_get_type;
            if (!ok)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] camera mode layout not found, camera position tuning off\n"));
                return false;
            }
            m_map_layout = FScriptMap::GetScriptLayout(1, 1, m_off.offset_size, m_off.offset_align); // ECameraType key: one byte
            m_layout_ok = true;
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] camera mode layout: offsets map 0x{:X}, CameraOffset {} bytes, value at 0x{:X}\n"),
                                           m_off.offsets_map, m_off.offset_size, m_map_layout.ValueOffset);
            return true;
        }

        static auto plausible(const Original& o) -> bool
        {
            if (!(o.fov >= 30.0f && o.fov <= 170.0f)) return false;
            if (!(o.pitch_min >= -90.0f && o.pitch_min <= 0.0f && o.pitch_max >= 0.0f && o.pitch_max <= 90.0f)) return false;
            if (o.offsets.empty() || o.offsets.size() > 3) return false;
            for (auto& off : o.offsets)
            {
                if (off.key < 1 || off.key > 3) return false;
                if (!(std::abs(off.x) <= 2000.0 && std::abs(off.y) <= 2000.0 && std::abs(off.z) <= 2000.0)) return false;
            }
            return true;
        }

        static auto read_float(UObject* object, int32_t offset) -> float
        {
            float v{};
            memcpy(&v, reinterpret_cast<uint8_t*>(object) + offset, sizeof(v));
            return v;
        }

        static auto write_float(UObject* object, int32_t offset, float value) -> void
        {
            memcpy(reinterpret_cast<uint8_t*>(object) + offset, &value, sizeof(value));
        }

        // m_live gains a live object if it is one of the captured modes and not held yet.
        auto keep_if_mode(LiveRef ref) -> bool
        {
            for (size_t i = 0; i < m_modes.size(); ++i)
            {
                if (!m_modes[i].captured || m_modes[i].cls != ref.cls || ref.object == m_modes[i].cdo) continue;
                bool held = std::any_of(m_live.begin(), m_live.end(), [&](auto& entry) { return entry.first.object == ref.object; });
                if (!held) m_live.push_back({ref, i});
                return true;
            }
            return false;
        }

        // The player's live modes: objects whose outer is the camera and whose class is a captured mode. It reads
        // every object (24 ms measured; FindAllOf's name compares took 60), so it runs once per camera or world,
        // where a load hides it, and note_new() supplies modes pushed later. Other cameras' modes are left
        // alone: new instances copy the CDO, and a new player camera gets its own apply.
        auto scan_instances(UObject* player_camera) -> void
        {
            drop_dead_classes();
            m_live.clear();
            if (!player_camera) return;
            UObjectGlobals::ForEachUObject([&](UObject* object, int32, int32) {
                if (object->GetOuterPrivate() == player_camera) keep_if_mode(LiveRef::of(object));
                return LoopAction::Continue;
            });
            m_scan_needed = false;
        }

        // Game thread. Takes what note_new() handed over and drops collected modes. After an overflow the
        // list may have missed a mode, so the next apply scans.
        auto adopt_new() -> void
        {
            std::vector<LiveRef> fresh;
            {
                std::lock_guard guard(m_new_mutex);
                fresh.swap(m_new);
                if (m_new_overflow) m_scan_needed = true;
                m_new_overflow = false;
            }
            drop_dead_classes();
            std::erase_if(m_live, [](auto& entry) { return !entry.first.alive(); });
            for (auto& ref : fresh)
            {
                if (ref.alive() && !keep_if_mode(ref)) adopt_late(ref);
            }
        }

        // A mode whose class loaded after the last apply (CombatSprinting is loaded by day only): its
        // CDO and this first instance still hold the game's values. The name is compared first, so the effect
        // cameras pushed on the same camera cost no object lookup.
        auto adopt_late(LiveRef ref) -> void
        {
            if (!m_applied) return;
            auto name = ref.cls->GetName();
            for (auto& mode : m_modes)
            {
                if (mode.captured || !mode.usable) continue;
                if (name != std::wstring(L"BP_CameraMode_") + mode.spec.name + L"_C") continue;
                mode.missing = false;
                capture();
                Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] {} loaded late: {}\n"), mode.spec.name, mode.captured ? STR("tuned") : STR("not found"));
                if (!mode.captured || !keep_if_mode(ref)) return;
                write(mode.cdo, mode, m_last);
                write(ref.object, mode, m_last);
                if (m_last.active) request_flip(ref.object->GetOuterPrivate());
                return;
            }
        }

        // LiveRefs, not pointers: a popped mode can be collected at any GC.
        template <typename Visit>
        auto for_each_instance(Visit&& visit) -> void
        {
            drop_dead_classes();
            for (auto& [ref, index] : m_live)
            {
                auto& mode = m_modes[index];
                if (!mode.captured || mode.cls != ref.cls || !ref.alive()) continue;
                visit(ref.object, mode);
            }
        }

        // transition: the flip's blend time; otherwise each mode's own.
        auto set_blend(bool transition) -> void
        {
            for_each_instance([&](UObject* instance, Mode& mode) {
                write_float(instance, m_off.type_blend, transition ? m_transition : mode.original.type_blend);
            });
        }

        template <typename Visit>
        auto for_each_offset(UObject* object, Visit&& visit) -> void
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

        auto read_original(UObject* cdo, Original& out) -> void
        {
            out.fov = read_float(cdo, m_off.fov);
            out.pitch_min = read_float(cdo, m_off.pitch_min);
            out.pitch_max = read_float(cdo, m_off.pitch_max);
            out.hlag_on = m_hlag_on->GetPropertyValueInContainer(cdo);
            out.vlag_on = m_vlag_on->GetPropertyValueInContainer(cdo);
            out.type_blend = read_float(cdo, m_off.type_blend);
            out.offsets.clear();
            for_each_offset(cdo, [&](uint8_t key, uint8_t* value) {
                OffsetOriginal o{key};
                double target[3]{};
                memcpy(target, value + m_off.offset_target, sizeof(target));
                o.x = target[0];
                o.y = target[1];
                o.z = target[2];
                memcpy(&o.overridden_fov, value + m_off.offset_fov, sizeof(float));
                out.offsets.push_back(o);
            });
        }

        auto write(UObject* object, const Mode& mode, const PositionTuning& t) -> void
        {
            const auto& o = mode.original;
            const auto& g = t.groups[mode.spec.group];
            bool on = t.active;

            write_float(object, m_off.fov, on ? static_cast<float>(o.fov + g.fov) : o.fov);
            // The game reads these every frame (tested live, 2026-09-19): off, the follow is the only lag.
            m_hlag_on->SetPropertyValueInContainer(object, o.hlag_on && !t.own_lag);
            m_vlag_on->SetPropertyValueInContainer(object, o.vlag_on && !t.own_lag);
            // Aiming and combat ship wider limits; only the -60 / 40 modes take the setting.
            bool game_range = o.pitch_min == -60.0f && o.pitch_max == 40.0f;
            write_float(object, m_off.pitch_min, on && game_range ? static_cast<float>(t.pitch_min) : o.pitch_min);
            write_float(object, m_off.pitch_max, on && game_range ? static_cast<float>(t.pitch_max) : o.pitch_max);

            for_each_offset(object, [&](uint8_t key, uint8_t* value) {
                for (auto& oo : o.offsets)
                {
                    if (oo.key != key) continue;
                    double target[3]{oo.x, oo.y, oo.z};
                    float fov = oo.overridden_fov;
                    if (on)
                    {
                        if (oo.x < 0.0) target[0] = oo.x * g.distance / 100.0;
                        if (oo.y != 0.0) // centred modes stay centred
                        {
                            // A negative offset stops at the centre instead of crossing to the other shoulder.
                            target[1] = std::copysign(std::max(std::abs(oo.y) + g.shoulder, 0.0), oo.y);
                            if (t.shoulder_swap) target[1] = -target[1];
                        }
                        target[2] = oo.z + g.height;
                        fov = static_cast<float>(oo.overridden_fov + g.fov);
                    }
                    memcpy(value + m_off.offset_target, target, sizeof(target));
                    memcpy(value + m_off.offset_fov, &fov, sizeof(fov));
                    break;
                }
            });
        }

        auto get_camera_type(UObject* camera) -> uint8_t
        {
            uint8_t params[16]{};
            camera->ProcessEvent(m_get_type, params);
            return params[0];
        }

        auto set_camera_type(UObject* camera, uint8_t type) -> void
        {
            uint8_t params[16]{};
            params[0] = type;
            camera->ProcessEvent(m_set_type, params);
        }

        // A request while away runs again after the switch back, so the latest values are read. One during the
        // glide back restarts at once: queued behind the glide, a quick second press moved the camera in two steps.
        auto request_flip(UObject* camera) -> void
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
    };
} // namespace dwsc
