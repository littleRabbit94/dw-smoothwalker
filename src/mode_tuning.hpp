// Camera position tuning: per-group distance, height, shoulder and FOV, and the look limits, plus the
// switch for the game's own camera lag, written into the game's camera modes. Game thread only, except note_new() (any thread) and
// restore() at unload.
//
// Every write is computed from a per-mode base, never from the current value, so applies cannot compound.
// The base starts as the CDO values captured the first time a class is seen; a value another mod wrote
// replaces that field of it (adopt_foreign()). CameraOffsets is only re-read on a camera type change, so an
// apply flips the player's camera type away and back (docs/design.md, "How writes land").
#pragma once

#include "config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
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

    inline auto group_name(Group group) -> const wchar_t*
    {
        static constexpr const wchar_t* names[GroupCount]{L"Exploration", L"Sprint", L"Combat", L"Focus", L"Aiming", L"Traversal"};
        return group >= 0 && group < GroupCount ? names[group] : L"?";
    }

    struct ModeClassSpec
    {
        Group group;
        const wchar_t* name;         // BP_CameraMode_<name>
        const wchar_t* folder = L""; // under Modes/, with its slash
        // Takes the pitch_min / pitch_max settings. The modes shipped at -60 / 40 (measured 2026-09-24 over all
        // 23 CDOs); the rest ship -89 / 89 (AimingOnLadder -40 / 89) and keep their own. A name list rather
        // than a test on the captured values, so a mod that writes limits before the first capture cannot
        // change which modes the setting applies to.
        bool player_pitch = false;
    };

    // Finisher and shadowstep attack cameras are scripted shots and stay as shipped.
    inline const std::vector<ModeClassSpec>& mode_classes()
    {
        constexpr bool pitch = true;
        static const std::vector<ModeClassSpec> specs{
                {Exploration, L"Base", L"", pitch}, {Exploration, L"Base_LongRange", L"", pitch},
                {Exploration, L"Base_CloseRange", L"", pitch}, {Exploration, L"Base_CloseRange_Mantle2m", L"", pitch},
                {Exploration, L"GapSqueeze", L"", pitch},
                {Sprint, L"Sprint", L"", pitch}, {Sprint, L"Sprint_VampiricFastTraversal", L"", pitch},
                {Combat, L"CombatNear"}, {Combat, L"CombatFromArm"}, {Combat, L"CombatFromArm_LongRange"},
                {Combat, L"CombatFromArm_VeryLongRange"}, {Combat, L"CombatFistFightMode"}, {Focus, L"FocusMode", L"", pitch},
                {Combat, L"CombatSprinting"},
                {Aiming, L"Aiming"}, {Aiming, L"AimingOnLadder"}, {Aiming, L"AimingClawRide"}, {Aiming, L"AimingClawRideLedge"},
                {Aiming, L"AntiGravAiming"},
                {Traversal, L"AntiGrav"}, {Traversal, L"ClawRide"}, {Traversal, L"ClawRideLedge"},
                {Traversal, L"Shadowstep_2_Base", L"Shadowstep/", pitch},
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

    // One live mode on the player's camera, for the debug overlay.
    struct ModeListing
    {
        std::wstring name; // BP_CameraMode_<name>_C, or the class name without _C
        uint8_t state;     // ECameraModeState: 0 blending in, 1 active, 2 blending out
        int group;         // Group; -1 for a mode this mod does not track
        bool tuned;        // tracked, and the last apply wrote the camera position into it
    };

    class ModeTuner
    {
      public:
        // Shipped values and the base are kept for the session: a class that stays loaded across a map load
        // still holds the mod's values and must not have them captured as the game's.
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
                        g_log_verbose.load(std::memory_order_relaxed))
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

        // Without a LoadMap hook, forget() may not run before a class unloads; every use of a captured CDO or
        // class pointer is preceded by this check: capture(), scan_instances(), adopt_foreign(), for_each_live()
        // (hence for_each_instance() and set_blend()).
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
            m_untracked.clear();
            m_scan_needed = true;
        }

        // Whether an Aiming-, Combat- or Traversal-group mode is blending in or active (ECameraModeState 0 or 1;
        // 2 blending out, 3 popped). One GetState call per live instance of those three groups per engine tick,
        // skipped once a group's flag is already set. A rescan request (a new camera or world, or an overflow in
        // adopt_new) is served here on the next tick, not left to an apply that may never come. Only then does it
        // capture and walk every object; every other tick only adopts the hand-off. capture() on every tick would
        // retry a class dropped by an unload each time (about 50 ms a failed lookup) and capture a reloaded one
        // without the late-class write adopt_late() gives it.
        auto mode_state(UObject* player_camera) -> ModeState
        {
            ModeState state;
            if (m_scan_needed) ensure_scanned(player_camera);
            else adopt_new();
            if (!m_layout_ok || !m_get_state || m_scan_needed) return state;
            for_each_instance([&](UObject* instance, Mode& mode) {
                bool* found = mode.spec.group == Aiming      ? &state.aiming
                              : mode.spec.group == Combat    ? &state.combat
                              : mode.spec.group == Traversal ? &state.traversal
                                                             : nullptr;
                if (!found || *found) return;
                *found = get_state(instance) <= 1;
            });
            return state;
        }

        // Every live mode on the player's camera with its state, newest first, popped ones (3) left out: the tracked
        // modes and the other RebelCameraMode subclasses the hand-off and the scan found (m_untracked). For the debug
        // overlay's refresh, a quarter second apart, never per tick: one GetState call per listed mode, and no object
        // walk. `stale`: a rescan is pending (a new camera or world, or a hand-off overflow), served by mode_state()
        // or the next apply, so the list may be short until then.
        auto list_modes(UObject* player_camera, bool& stale) -> std::vector<ModeListing>
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

        // The player's camera type (ECameraType) by its enum name when the return value's enum is reflected, else the
        // number; empty before the layout is resolved; "n/a" when the camera is not a RebelCameraComponent (the function
        // would run on the wrong class). Game thread, one GetCameraType call: the debug overlay's refresh.
        auto camera_type_name(UObject* player_camera) -> std::wstring
        {
            if (!m_get_type || !player_camera) return {};
            if (!m_camera_class_read)
            {
                m_camera_class_read = true;
                m_camera_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/RebelCamera.RebelCameraComponent"));
            }
            auto* cls = player_camera->GetClassPrivate();
            if (!m_camera_class || !cls || !cls->IsChildOf(m_camera_class)) return L"n/a";
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
            if (g_log_verbose.load(std::memory_order_relaxed))
            {
                auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] camera position applied: {} mode classes, {} live modes, {:.1f} ms\n"), classes, live, ms);
            }
        }

        // At unload, after the mod's callbacks are gone. The CDOs get each mode's base, so values another mod
        // wrote since the last apply stay. Live instances keep their values until the game pushes new ones.
        auto restore() -> void
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
            bool has_shipped = false;
            bool usable = true;
            bool keys_warned = false; // the CameraOffsets key-set warning, once per session
            Original shipped; // the first capture, never changed: for the logs and adopt_foreign()
            // What writes are computed from: shipped, with each value another mod wrote in its place. type_blend
            // and the lag bits stay as captured: the mod writes those itself (the flip, own_lag).
            Original base;
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
        // The player's live modes, with their m_modes index and the order they were taken in (m_seq): the scan takes
        // what is already there, the hand-off what is pushed later, so a higher seq is a newer mode.
        // known: its values can be classified by adopt_foreign(). A scan finds modes whose values predate what this
        // tuner can tell apart (a hot reload leaves the old DLL's writes in them, a new pawn's camera an older
        // apply's), so they are not checked until an apply or adopt_late() writes them. A hand-off mode was built
        // from a CDO that is checked, so it is known from the start.
        struct LiveMode
        {
            LiveRef ref;
            size_t index;
            uint64_t seq;
            bool known = false;
        };
        std::vector<LiveMode> m_live;
        // RebelCameraMode instances on the player's camera whose class is not in mode_classes() (shadowstep attack,
        // finisher and effect cameras), each with its display name, taken like m_live. Only listed (list_modes).
        struct Untracked
        {
            LiveRef ref;
            std::wstring name;
            uint64_t seq;
        };
        std::vector<Untracked> m_untracked;
        static constexpr size_t MAX_UNTRACKED = 32;
        uint64_t m_seq = 0;
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
        UStruct* m_mode_base = nullptr;   // RebelCameraMode: what an untracked object must derive from to be listed
        UClass* m_camera_class = nullptr; // RebelCameraComponent: camera_type_name() calls GetCameraType only on one
        bool m_camera_class_read = false;
        bool m_type_names_read = false;   // camera_type_name(): the enum names, read once
        int32_t m_type_offset = -1;       // GetCameraType's return value in its params
        std::vector<std::pair<int64, std::wstring>> m_type_names;

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
            if (g_log_verbose.load(std::memory_order_relaxed))
            {
                Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] camera mode layout: offsets map 0x{:X}, CameraOffset {} bytes, value at 0x{:X}\n"),
                                               m_off.offsets_map, m_off.offset_size, m_map_layout.ValueOffset);
            }
            return true;
        }

        // foreign: a value another mod wrote. Only those take the override FOV bound, which no shipped value has
        // been measured against, so a first capture is gated as before.
        static auto plausible(const Original& o, bool foreign = false) -> bool
        {
            if (!(o.fov >= 30.0f && o.fov <= 170.0f)) return false;
            if (!(o.pitch_min >= -90.0f && o.pitch_min <= 0.0f && o.pitch_max >= 0.0f && o.pitch_max <= 90.0f)) return false;
            if (o.offsets.empty() || o.offsets.size() > 3) return false;
            for (auto& off : o.offsets)
            {
                if (off.key < 1 || off.key > 3) return false;
                if (!(std::abs(off.x) <= 2000.0 && std::abs(off.y) <= 2000.0 && std::abs(off.z) <= 2000.0)) return false;
                // 0 where the key has no override; the ceiling is DefaultFieldOfView's. Also rejects NaN.
                if (foreign && !(off.overridden_fov >= 0.0f && off.overridden_fov <= 170.0f)) return false;
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

        // m_live gains a live object if it is one of the captured modes and not held yet. known: from the hand-off
        // (see LiveMode); a held mode the hand-off brings again becomes known. A rescan clears m_live, so every mode
        // it finds starts unknown again.
        auto keep_if_mode(LiveRef ref, bool known) -> bool
        {
            for (size_t i = 0; i < m_modes.size(); ++i)
            {
                if (!m_modes[i].captured || m_modes[i].cls != ref.cls || ref.object == m_modes[i].cdo) continue;
                auto held = std::find_if(m_live.begin(), m_live.end(), [&](auto& entry) { return entry.ref.object == ref.object; });
                if (held == m_live.end()) m_live.push_back({ref, i, ++m_seq, known});
                else held->known = held->known || known;
                return true;
            }
            return false;
        }

        // An untracked RebelCameraMode on the player's camera joins m_untracked. The class is checked first, so the
        // camera's other subobjects cost no name. Full: the dead are dropped, then the oldest.
        auto keep_untracked(LiveRef ref) -> void
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

        // The player's live modes: objects whose outer is the camera and whose class is a captured mode, and the
        // other camera modes on it into m_untracked. It reads every object (24 ms measured; FindAllOf's name
        // compares took 60), so it runs once per camera or world, where a load hides it, and note_new() supplies
        // modes pushed later. Other cameras' modes are left alone: new instances copy the CDO, and a new player
        // camera gets its own apply.
        auto scan_instances(UObject* player_camera) -> void
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

        // Capture first: after forget() nothing is captured, and a scan then would find no modes yet clear
        // m_scan_needed, leaving the apply to write the CDOs only.
        auto ensure_scanned(UObject* player_camera) -> void
        {
            capture();
            if (!m_layout_ok) return;
            adopt_new();
            if (m_scan_needed) scan_instances(player_camera);
        }

        // Game thread. Takes what note_new() handed over and drops collected modes. After an overflow the
        // list may have missed a mode, so the next mode_state() or apply scans.
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
            std::erase_if(m_live, [](auto& entry) { return !entry.ref.alive(); });
            for (auto& ref : fresh)
            {
                if (ref.alive() && !keep_if_mode(ref, true) && !adopt_late(ref)) keep_untracked(ref);
            }
        }

        // A mode whose class loaded after the last apply (CombatSprinting is loaded by day only): its
        // CDO and this first instance still hold the game's values, or, for a class captured before a map load,
        // the shipped ones or another mod's, so they are checked (adopt_from()) before the write. The name is
        // compared first, so the effect cameras pushed on the same camera cost no object lookup. True if the
        // instance joined m_live.
        auto adopt_late(LiveRef ref) -> bool
        {
            if (!m_applied) return false;
            auto name = ref.cls->GetName();
            for (auto& mode : m_modes)
            {
                if (mode.captured || !mode.usable) continue;
                if (name != std::wstring(L"BP_CameraMode_") + mode.spec.name + L"_C") continue;
                mode.missing = false;
                capture();
                if (g_log_verbose.load(std::memory_order_relaxed))
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

        // LiveRefs, not pointers: a popped mode can be collected at any GC.
        template <typename Visit>
        auto for_each_live(Visit&& visit) -> void
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
        auto for_each_instance(Visit&& visit) -> void
        {
            for_each_live([&](LiveMode& live, Mode& mode) { visit(live.ref.object, mode); });
        }

        // transition: the flip's blend time; otherwise each mode's own.
        auto set_blend(bool transition) -> void
        {
            for_each_instance([&](UObject* instance, Mode& mode) {
                write_float(instance, m_off.type_blend, transition ? m_transition : mode.base.type_blend);
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

        // A CDO's or a live instance's current values: memory reads only.
        auto read_values(UObject* object, Original& out) -> void
        {
            out.fov = read_float(object, m_off.fov);
            out.pitch_min = read_float(object, m_off.pitch_min);
            out.pitch_max = read_float(object, m_off.pitch_max);
            out.hlag_on = m_hlag_on->GetPropertyValueInContainer(object);
            out.vlag_on = m_vlag_on->GetPropertyValueInContainer(object);
            out.type_blend = read_float(object, m_off.type_blend);
            out.offsets.clear();
            for_each_offset(object, [&](uint8_t key, uint8_t* value) {
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

        // What write() puts into a mode for a tuning, computed from its base: the one computation behind the
        // writes and behind telling the mod's own values from another mod's (adopt_foreign()). type_blend is
        // carried over, not written: the flip owns it.
        static auto expected(const Mode& mode, const PositionTuning& t) -> Original
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
            for (auto& off : e.offsets) // each field from its own base value, so in place
            {
                if (off.x < 0.0) off.x = off.x * g.distance / 100.0;
                if (off.y != 0.0) // centred modes stay centred
                {
                    // A negative offset stops at the centre instead of crossing to the other shoulder.
                    off.y = std::copysign(std::max(std::abs(off.y) + g.shoulder, 0.0), off.y);
                    if (t.shoulder_swap) off.y = -off.y;
                }
                off.z = off.z + g.height;
                off.overridden_fov = static_cast<float>(off.overridden_fov + g.fov);
            }
            return e;
        }

        auto write(UObject* object, const Mode& mode, const PositionTuning& t) -> void
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
                    break;
                }
            });
        }

        // Bitwise: every value the mod wrote went in verbatim, and a NaN must still equal itself.
        template <typename T>
        static auto same(T a, T b) -> bool
        {
            return std::memcmp(&a, &b, sizeof(T)) == 0;
        }

        static auto same_keys(const Original& a, const Original& b) -> bool
        {
            if (a.offsets.size() != b.offsets.size()) return false;
            return std::all_of(a.offsets.begin(), a.offsets.end(), [&](auto& off) {
                return std::any_of(b.offsets.begin(), b.offsets.end(), [&](auto& other) { return other.key == off.key; });
            });
        }

        static auto camera_type_label(uint8_t key) -> const wchar_t*
        {
            return key == 1 ? L"Default" : key == 2 ? L"Interior" : key == 3 ? L"Habitat" : L"?";
        }

        // Values another mod wrote into a captured CDO or a live instance become that field of the mode's base, so
        // the writes after it apply the player's settings on top of them and restore() leaves them. A value is the
        // mod's own if it is the last write (expected() of the last tuning), the base (an instance copied from the
        // CDO, or an inactive apply) or the shipped value (a class that reloaded after a map load); anything else
        // is foreign. Only known live modes are read (LiveMode). Memory reads only, no UObject calls and no object
        // walk: at the start of apply() and restore(), and for one class in adopt_late(); never per tick. CDOs are
        // read before instances, and where they carry different foreign values the last one read wins; a value
        // that reads as the mod's own changes nothing.
        auto adopt_foreign() -> void
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

        auto adopt_from(Mode& mode, const std::vector<Original>& seen) -> void
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
            // field: 0-2 the scalars, 3 + 4 * key index + axis the offsets; the out-of-range warning once per field
            // per check, not once per object holding the value. get: one field of an Original. label: that field
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
                    int field = 3 + 4 * static_cast<int>(k); // plausible() allows at most 3 keys: bits 3-14
                    consider(field, found->x, [k](auto& o) -> auto& { return o.offsets[k].x; }, offset(L"X offset"));
                    consider(field + 1, found->y, [k](auto& o) -> auto& { return o.offsets[k].y; }, offset(L"Y offset"));
                    consider(field + 2, found->z, [k](auto& o) -> auto& { return o.offsets[k].z; }, offset(L"Z offset"));
                    consider(field + 3, found->overridden_fov, [k](auto& o) -> auto& { return o.offsets[k].overridden_fov; }, offset(L"FOV override"));
                }
            }
        }

        // ECameraModeState; 0xFF if the call wrote nothing.
        auto get_state(UObject* mode) -> uint8_t
        {
            uint8_t params[16]{};
            params[0] = 0xFF;
            mode->ProcessEvent(m_get_state, params);
            return params[0];
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
