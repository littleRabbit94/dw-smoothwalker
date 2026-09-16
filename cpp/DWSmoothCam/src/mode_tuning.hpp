// Camera position tuning: per-group distance, height, shoulder and FOV, the game's own lag and the look
// limits, written into the game's camera modes. Game thread only, except restore() at unload.
//
// Every write is computed from the CDO values captured the first time a class is seen, never from the
// current value, so applies cannot compound. CameraOffsets is only re-read on a camera type change, so an
// apply flips the player's camera type away and back (docs/design.md, "How writes land").
#pragma once

#include "config.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dwsc
{
    using namespace RC;
    using namespace RC::Unreal;

    enum Group : int
    {
        Exploration,
        Sprint,
        Combat,
        Aiming,
        Traversal
    };

    struct ModeClassSpec
    {
        Group group;
        const wchar_t* name; // BP_CameraMode_<name>
    };

    // Finisher and shadowstep attack cameras are scripted shots and stay as shipped.
    inline const std::vector<ModeClassSpec>& mode_classes()
    {
        static const std::vector<ModeClassSpec> specs{
                {Exploration, L"Base"}, {Exploration, L"Base_LongRange"}, {Exploration, L"Base_CloseRange"},
                {Exploration, L"Base_CloseRange_Mantle2m"}, {Exploration, L"GapSqueeze"},
                {Sprint, L"Sprint"}, {Sprint, L"Sprint_VampiricFastTraversal"},
                {Combat, L"CombatNear"}, {Combat, L"CombatFromArm"}, {Combat, L"CombatFromArm_LongRange"},
                {Combat, L"CombatFromArm_VeryLongRange"}, {Combat, L"CombatFistFightMode"}, {Combat, L"FocusMode"},
                {Aiming, L"Aiming"}, {Aiming, L"AimingOnLadder"}, {Aiming, L"AimingClawRide"}, {Aiming, L"AimingClawRideLedge"},
                {Aiming, L"AntiGravAiming"},
                {Traversal, L"AntiGrav"}, {Traversal, L"ClawRide"}, {Traversal, L"ClawRideLedge"},
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
        bool active = true;
        GroupTuning groups[5];
        double game_lag_scale = 1;
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
        for (int i = 0; i < 5; ++i)
        {
            if (!(a.groups[i] == b.groups[i])) return false;
        }
        return a.active == b.active && a.game_lag_scale == b.game_lag_scale && a.shoulder_swap == b.shoulder_swap &&
               a.pitch_min == b.pitch_min && a.pitch_max == b.pitch_max && a.transition == b.transition;
    }

    inline auto position_of(const Settings& s) -> PositionTuning
    {
        PositionTuning p;
        p.active = s.camera_tuning;
        p.groups[Exploration] = {s.exploration_distance, s.exploration_height, s.exploration_shoulder, s.exploration_fov};
        p.groups[Sprint] = {s.sprint_distance, s.sprint_height, s.sprint_shoulder, s.sprint_fov};
        p.groups[Combat] = {s.combat_distance, s.combat_height, s.combat_shoulder, s.combat_fov};
        p.groups[Aiming] = {s.aiming_distance, s.aiming_height, s.aiming_shoulder, s.aiming_fov};
        p.groups[Traversal] = {s.traversal_distance, s.traversal_height, 0.0, s.traversal_fov};
        p.game_lag_scale = s.game_lag_scale;
        p.shoulder_swap = s.shoulder_swap;
        p.pitch_min = s.pitch_min;
        p.pitch_max = s.pitch_max;
        p.transition = s.position_transition;
        return p;
    }

    class ModeTuner
    {
      public:
        // Originals are kept for the session: a class that stays loaded across a map load still holds the
        // mod's values and must not have them captured as the game's.
        auto capture() -> void
        {
            if (!m_layout_ok && !resolve_layout()) return;
            for (auto& mode : m_modes)
            {
                if (mode.captured || !mode.usable) continue;
                auto path = std::wstring(L"/Game/_Dawnwalker/Player/Camera/Modes/BP_CameraMode_") + mode.spec.name + L".Default__BP_CameraMode_" +
                            mode.spec.name + L"_C";
                auto* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
                if (!cdo) continue;
                if (!mode.has_original)
                {
                    read_original(cdo, mode.original);
                    // A wrong layout would put every write in the wrong memory.
                    if (!plausible(mode.original))
                    {
                        Output::send<LogLevel::Warning>(STR("[DWSmoothCam] {} reads as fov {}, {} offsets: not a camera layout, left as shipped\n"),
                                                        mode.spec.name, mode.original.fov, mode.original.offsets.size());
                        mode.usable = false;
                        continue;
                    }
                    mode.has_original = true;
                    if (std::wstring_view(mode.spec.name) == L"Base_LongRange" && !mode.original.offsets.empty())
                    {
                        auto& first = mode.original.offsets.front();
                        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] Base_LongRange: fov {}, lag {}/{}, pitch {}/{}, {} offsets, key {} at ({}, {}, {})\n"),
                                                       mode.original.fov, mode.original.hlag, mode.original.vlag, mode.original.pitch_min,
                                                       mode.original.pitch_max, mode.original.offsets.size(), first.key, first.x, first.y, first.z);
                    }
                }
                mode.cdo = cdo;
                mode.cls = cdo->GetClassPrivate();
                mode.captured = true;
            }
        }

        // Before a map load: classes may unload.
        auto forget() -> void
        {
            for (auto& mode : m_modes)
            {
                mode.captured = false;
                mode.cdo = nullptr;
                mode.cls = nullptr;
            }
            m_flip_stage = Idle;
            m_flip_again = false;
            m_flip_camera = nullptr;
        }

        auto apply(const PositionTuning& tuning, UObject* player_camera) -> void
        {
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
            for_each_instance([&](UObject* instance, Mode& mode) {
                write(instance, mode, tuning);
                ++live;
            });
            m_applied = true;
            m_transition = static_cast<float>(tuning.transition);
            request_flip(player_camera);
            Output::send<LogLevel::Normal>(STR("[DWSmoothCam] camera position applied: {} mode classes, {} live modes\n"), classes, live);
        }

        // At unload, after the mod's callbacks are gone. Live instances keep their values until the game pushes
        // new ones.
        auto restore() -> void
        {
            if (!m_applied) return;
            capture(); // forget() may have dropped the CDO pointers since the last apply
            if (!m_layout_ok) return;
            PositionTuning neutral;
            neutral.active = false;
            for (auto& mode : m_modes)
            {
                if (mode.captured) write(mode.cdo, mode, neutral);
            }
        }

        // Once per engine tick. A stage advances only after the player's camera has updated, so a flip
        // requested under a pause (the Mod Menu) waits for the world instead of running unseen.
        auto tick(uint64_t view_updates, UObject* player_camera) -> void
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
                m_flip_back_at = std::chrono::steady_clock::now();
                m_flip_stage = Back;
                return;
            case Back: {
                // The running blend keeps the settings it started with.
                auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_flip_back_at).count();
                if (elapsed < m_transition + 0.25) return;
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
            float fov{}, hlag{}, vlag{}, pitch_min{}, pitch_max{}, type_blend{};
            std::vector<OffsetOriginal> offsets;
        };

        struct Mode
        {
            ModeClassSpec spec;
            UObject* cdo = nullptr;
            UClass* cls = nullptr;
            bool captured = false;
            bool has_original = false;
            bool usable = true;
            Original original;
        };

        struct Offsets
        {
            int32_t fov = -1, pitch_min = -1, pitch_max = -1, vlag = -1, hlag = -1;
            int32_t offsets_map = -1, type_blend = -1; // type_blend: CameraTypeBlendArgs.BlendTime
            int32_t offset_target = -1, offset_fov = -1;
            int32_t offset_size = 0, offset_align = 0;
        };

        std::vector<Mode> m_modes = [] {
            std::vector<Mode> modes;
            for (auto& spec : mode_classes()) modes.push_back({spec});
            return modes;
        }();
        Offsets m_off;
        bool m_layout_ok = false;
        bool m_layout_tried = false;
        bool m_applied = false;
        FScriptMapLayout m_map_layout{};

        UFunction* m_set_type = nullptr;
        UFunction* m_get_type = nullptr;

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
        std::chrono::steady_clock::time_point m_flip_back_at{};

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

            m_off.fov = offset_in(mode, STR("DefaultFieldOfView"));
            m_off.pitch_min = offset_in(mode, STR("ViewPitchMin"));
            m_off.pitch_max = offset_in(mode, STR("ViewPitchMax"));
            m_off.vlag = offset_in(mode, STR("CameraVerticalLagSpeed"));
            m_off.hlag = offset_in(mode, STR("CameraHorizontalLagSpeed"));
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

            bool ok = m_off.fov >= 0 && m_off.pitch_min >= 0 && m_off.pitch_max >= 0 && m_off.vlag >= 0 && m_off.hlag >= 0 &&
                      m_off.offsets_map >= 0 && m_off.type_blend >= 0 && m_off.offset_target >= 0 && m_off.offset_fov >= 0 &&
                      m_off.offset_size > 0 && m_off.offset_align > 0 && m_set_type && m_get_type;
            if (!ok)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothCam] camera mode layout not found, camera position tuning off\n"));
                return false;
            }
            m_map_layout = FScriptMap::GetScriptLayout(1, 1, m_off.offset_size, m_off.offset_align); // ECameraType key: one byte
            m_layout_ok = true;
            Output::send<LogLevel::Normal>(STR("[DWSmoothCam] camera mode layout: offsets map 0x{:X}, CameraOffset {} bytes, value at 0x{:X}\n"),
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

        // Live instances are looked up each time rather than kept: a popped mode can be collected at any GC.
        template <typename Visit>
        auto for_each_instance(Visit&& visit) -> void
        {
            std::vector<UObject*> instances;
            UObjectGlobals::FindAllOf(STR("RebelCameraModeTPP"), instances);
            for (auto* instance : instances)
            {
                if (!instance) continue;
                auto* cls = instance->GetClassPrivate();
                for (auto& mode : m_modes)
                {
                    if (!mode.captured || mode.cls != cls || instance == mode.cdo) continue;
                    visit(instance, mode);
                    break;
                }
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
            out.vlag = read_float(cdo, m_off.vlag);
            out.hlag = read_float(cdo, m_off.hlag);
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
            write_float(object, m_off.hlag, on ? static_cast<float>(o.hlag * t.game_lag_scale) : o.hlag);
            write_float(object, m_off.vlag, on ? static_cast<float>(o.vlag * t.game_lag_scale) : o.vlag);
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

        // A request during a running flip runs again after it, so the latest values are the ones read.
        auto request_flip(UObject* camera) -> void
        {
            if (!camera) return;
            if (m_flip_stage == Idle)
            {
                m_flip_camera = camera;
                m_flip_stage = Pending;
            }
            else if (m_flip_stage != Pending) m_flip_again = true;
        }
    };
} // namespace dwsc
