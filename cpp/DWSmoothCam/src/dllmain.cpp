// DWSmoothCam: frame-interpolated third-person camera for The Blood of Dawnwalker.
// Hooks RebelCameraComponent::GetCameraView (vtable slot 214), lets the game build its view, then lags
// the character pivot the view is built around and adds that lag to the camera. The game's collision,
// offsets, FOV and blends stay the game's.
// Design notes: docs/design.md, "The gameplay camera" and "The per-frame hook".
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "config.hpp"
#include "smoothing.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace
{
    constexpr size_t GET_CAMERA_VIEW_SLOT = 214;
    constexpr const char* CONFIG_PATH = "ue4ss/Mods/DWSmoothCam/scripts/config/smoothcam.ini";

    // Leading members of UE 5.5 FMinimalViewInfo: Location, Rotation (Pitch, Yaw, Roll).
    struct ViewHead
    {
        double location[3];
        double rotation[3];
    };

    using GetCameraViewFn = void(__fastcall*)(void* self, float delta_time, void* desired_view);

    dwsc::Settings g_settings;
    GetCameraViewFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;

    // Published by the game thread, read by the hook. Pointers are only compared or read under SEH.
    std::atomic<bool> g_enabled{true};
    std::atomic<bool> g_reset{true};
    std::atomic<void*> g_player_camera{nullptr};
    std::atomic<void*> g_player_root{nullptr};
    std::atomic<int32_t> g_translation_offset{-1}; // USceneComponent::ComponentToWorld.Translation

    // Stats for log_stats.
    std::atomic<uint64_t> g_frames{0};
    std::atomic<uint64_t> g_clamped{0};
    std::atomic<double> g_lag_sum{0.0};

    // Kept free of C++ objects: __try needs a plain frame.
    auto guarded_read(void* from, void* to, size_t bytes) -> bool
    {
        __try
        {
            memcpy(to, from, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto guarded_write(void* to, const void* from, size_t bytes) -> bool
    {
        __try
        {
            memcpy(to, from, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Hook-thread state. GetCameraView calls for one camera arrive in sequence, and only the player's
    // camera reaches this, so no lock.
    struct Follow
    {
        bool valid = false;
        dwsc::Vec3 pivot_smoothed{};
        dwsc::Vec3 pivot_last{};
        dwsc::Quat rotation_smoothed{};
        double nominal_distance = 0.0;
        LARGE_INTEGER last_call{};
    };
    Follow g_follow;
    LARGE_INTEGER g_qpc_frequency{};

    auto seconds_between(LARGE_INTEGER a, LARGE_INTEGER b) -> double
    {
        return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g_qpc_frequency.QuadPart);
    }

    auto smooth_view(void* desired_view, float delta_time) -> void
    {
        const auto& s = g_settings;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        auto* root = g_player_root.load(std::memory_order_relaxed);
        auto offset = g_translation_offset.load(std::memory_order_relaxed);
        double pivot_raw[3]{};
        ViewHead view{};
        if (!root || offset < 0 || !guarded_read(static_cast<uint8_t*>(root) + offset, pivot_raw, sizeof(pivot_raw)) ||
            !guarded_read(desired_view, &view, sizeof(view)))
        {
            g_follow.valid = false;
            return;
        }

        dwsc::Vec3 pivot{pivot_raw[0], pivot_raw[1], pivot_raw[2]};
        dwsc::Vec3 camera{view.location[0], view.location[1], view.location[2]};
        dwsc::Quat rotation = dwsc::from_rotator(view.rotation[0], view.rotation[1], view.rotation[2]);

        // Snap after a toggle, a gap (cutscene, photo mode, load) or a teleport.
        bool snap = g_reset.exchange(false, std::memory_order_relaxed) || !g_follow.valid ||
                    seconds_between(g_follow.last_call, now) > s.reset_gap || dwsc::length(pivot - g_follow.pivot_last) > s.reset_distance;
        g_follow.last_call = now;
        g_follow.pivot_last = pivot;
        if (snap)
        {
            g_follow.valid = true;
            g_follow.pivot_smoothed = pivot;
            g_follow.rotation_smoothed = rotation;
            g_follow.nominal_distance = dwsc::length(camera - pivot);
            return;
        }

        // The world delta: 0 while paused, so a paused frame holds the lag instead of settling it.
        double dt = std::clamp(static_cast<double>(delta_time), 0.0, 0.1);

        // Horizontal and vertical follow, each with its own rate, curve and leash.
        dwsc::Vec3& ps = g_follow.pivot_smoothed;
        double lag_hx = pivot.x - ps.x, lag_hy = pivot.y - ps.y;
        double lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
        double a_h = dwsc::follow_alpha(s.follow_rate_h, s.curve_h, lag_h, s.catchup_distance, s.min_rate_scale, dt);
        ps.x += lag_hx * a_h;
        ps.y += lag_hy * a_h;
        lag_hx = pivot.x - ps.x;
        lag_hy = pivot.y - ps.y;
        lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
        if (lag_h > s.max_lag_h && lag_h > 0.0)
        {
            double k = s.max_lag_h / lag_h;
            ps.x = pivot.x - lag_hx * k;
            ps.y = pivot.y - lag_hy * k;
        }

        double lag_v = pivot.z - ps.z;
        double a_v = dwsc::follow_alpha(s.follow_rate_v, s.curve_v, std::abs(lag_v), s.catchup_distance, s.min_rate_scale, dt);
        ps.z += lag_v * a_v;
        lag_v = pivot.z - ps.z;
        if (std::abs(lag_v) > s.max_lag_v) ps.z = pivot.z - std::copysign(s.max_lag_v, lag_v);

        // Rotation: slerp toward the game's rotation, and swing the camera around the pivot to match.
        dwsc::Vec3 arm = camera - pivot;
        if (s.rotation_smoothing)
        {
            double a_r = 1.0 - std::exp(-std::max(s.rotation_rate, 0.0) * dt);
            g_follow.rotation_smoothed = dwsc::slerp(g_follow.rotation_smoothed, rotation, a_r);
            dwsc::Quat delta = dwsc::multiply(g_follow.rotation_smoothed, dwsc::conjugate(rotation));
            arm = dwsc::rotate(delta, arm);
            dwsc::to_rotator(g_follow.rotation_smoothed, view.rotation[0], view.rotation[1], view.rotation[2]);
        }
        else
        {
            g_follow.rotation_smoothed = rotation;
        }

        dwsc::Vec3 result = ps + arm;

        // Walls: the game already pulled its camera in front of any wall. While the camera is closer to
        // the pivot than it has recently been, keep the smoothed camera no farther out than the game's.
        double game_distance = dwsc::length(arm);
        double settle = 1.0 - std::exp(-1.0 * dt);
        g_follow.nominal_distance = std::max(game_distance, g_follow.nominal_distance + (game_distance - g_follow.nominal_distance) * settle);
        if (s.wall_clamp && game_distance < 0.85 * g_follow.nominal_distance)
        {
            dwsc::Vec3 out = result - pivot;
            double out_distance = dwsc::length(out);
            if (out_distance > game_distance && out_distance > 0.0)
            {
                result = pivot + out * (game_distance / out_distance);
                g_clamped.fetch_add(1, std::memory_order_relaxed);
            }
        }

        view.location[0] = result.x;
        view.location[1] = result.y;
        view.location[2] = result.z;
        if (!guarded_write(desired_view, &view, sizeof(view))) g_follow.valid = false;

        g_frames.fetch_add(1, std::memory_order_relaxed);
        g_lag_sum.store(g_lag_sum.load(std::memory_order_relaxed) + dwsc::length(pivot - ps), std::memory_order_relaxed);
    }

    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view)
    {
        g_original(self, delta_time, desired_view);
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        if (self != g_player_camera.load(std::memory_order_relaxed)) return;
        smooth_view(desired_view, delta_time);
    }

    auto parse_key(const std::string& name) -> int
    {
        if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        {
            return std::toupper(static_cast<unsigned char>(name[0]));
        }
        if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
        {
            int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) return 0x70 + n - 1;
        }
        return -1;
    }

    auto object_ptr(UObject* owner, const TCHAR* property) -> UObject*
    {
        if (!owner) return nullptr;
        auto** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(property);
        return value ? *value : nullptr;
    }
} // namespace

class DWSmoothCam : public CppUserModBase
{
  public:
    DWSmoothCam() : CppUserModBase()
    {
        ModName = STR("DWSmoothCam");
        ModVersion = STR("0.4.0");
        ModDescription = STR("Frame-interpolated third-person camera");
        ModAuthors = STR("littleRabbit6");

        QueryPerformanceFrequency(&g_qpc_frequency);
        g_settings = dwsc::load_settings(CONFIG_PATH);
        g_enabled.store(g_settings.enabled);
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] v{} loaded, {}\n"), ModVersion, g_settings.enabled ? STR("on") : STR("off"));
    }

    ~DWSmoothCam() override
    {
        // The DLL can be unloaded (hot reload): the vtable must not point into it afterwards.
        if (g_vtable_entry && g_original)
        {
            DWORD prev{};
            if (VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), PAGE_READWRITE, &prev))
            {
                *g_vtable_entry = reinterpret_cast<uintptr_t*>(g_original);
                VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), prev, &prev);
            }
        }
        for (auto id : m_callbacks) Hook::UnregisterCallback(id);
    }

    auto on_unreal_init() -> void override
    {
        m_player_controller_name = FName(STR("BP_PlayerController_C"), FNAME_Add);

        if (!install_hook()) return;

        Hook::FCallbackOptions options{false, true, STR("DWSmoothCam"), STR("")};
        m_callbacks.push_back(Hook::RegisterBeginPlayPostCallback(
                [this](auto&, AActor* actor) { on_begin_play(actor); }, options));
        m_callbacks.push_back(Hook::RegisterEndPlayPostCallback(
                [this](auto&, AActor* actor, EEndPlayReason) { on_end_play(actor); }, options));
        m_callbacks.push_back(Hook::RegisterLoadMapPreCallback(
                [this](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) { forget_player(); }, options));
        m_callbacks.push_back(Hook::RegisterEngineTickPostCallback(
                [this](auto&, UEngine*, float, bool) { on_engine_tick(); }, options));

        int key = parse_key(g_settings.toggle_key);
        if (key < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] unknown toggle_key, no toggle bound\n"));
        }
        else
        {
            register_keydown_event(static_cast<Input::Key>(key), [this]() {
                bool now = !g_enabled.load();
                g_reset.store(true);
                g_enabled.store(now);
                if (!m_controller) m_find_requested.store(true); // e.g. after a hot reload mid-game
                Output::send<LogLevel::Normal>(STR("[DWSmoothCam] smoothing {}\n"), now ? STR("on") : STR("off"));
            });
        }
        m_last_report = std::chrono::steady_clock::now();
    }

    auto on_update() -> void override
    {
        if (!g_settings.log_stats) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - m_last_report).count();
        if (elapsed < 5.0) return;
        m_last_report = now;
        auto frames = g_frames.exchange(0);
        auto clamped = g_clamped.exchange(0);
        auto lag = g_lag_sum.exchange(0.0);
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] {:.1f} smoothed frames/s, mean pivot lag {:.1f} cm, wall clamp {:.0f}%\n"),
                                       frames / elapsed, frames ? lag / frames : 0.0, frames ? 100.0 * clamped / frames : 0.0);
    }

  private:
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    FName m_player_controller_name{};
    UObject* m_controller = nullptr; // game thread only
    UObject* m_pawn = nullptr;
    int32_t m_pawn_offset = -1; // AController::Pawn, same class every map
    std::atomic<bool> m_find_requested{false};
    std::chrono::steady_clock::time_point m_last_report{};

    auto install_hook() -> bool
    {
        auto* camera = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__CameraComponent"));
        auto* rebel = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/RebelCamera.Default__RebelCameraComponent"));
        if (!camera || !rebel)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] camera class defaults not found, mod inactive\n"));
            return false;
        }
        auto** base = *reinterpret_cast<uintptr_t***>(camera);
        auto** vtable = *reinterpret_cast<uintptr_t***>(rebel);
        if (base[GET_CAMERA_VIEW_SLOT] == vtable[GET_CAMERA_VIEW_SLOT])
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] slot {} not overridden: unsupported game build, mod inactive\n"), GET_CAMERA_VIEW_SLOT);
            return false;
        }

        auto** entry = &vtable[GET_CAMERA_VIEW_SLOT];
        DWORD prev{};
        if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &prev))
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] vtable protect failed, mod inactive\n"));
            return false;
        }
        g_original = reinterpret_cast<GetCameraViewFn>(*entry);
        *entry = reinterpret_cast<uintptr_t*>(&get_camera_view_hook);
        VirtualProtect(entry, sizeof(*entry), prev, &prev);
        g_vtable_entry = entry;
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] GetCameraView hooked (slot {})\n"), GET_CAMERA_VIEW_SLOT);
        return true;
    }

    auto forget_player() -> void
    {
        g_player_camera.store(nullptr);
        g_player_root.store(nullptr);
        g_reset.store(true);
        m_controller = nullptr;
        m_pawn = nullptr;
    }

    auto on_begin_play(AActor* actor) -> void
    {
        auto* object = static_cast<UObject*>(actor);
        if (!object || m_controller) return;
        auto* cls = object->GetClassPrivate();
        if (cls && cls->GetNamePrivate() == m_player_controller_name)
        {
            m_controller = object;
            m_pawn = nullptr;
        }
    }

    auto on_end_play(AActor* actor) -> void
    {
        auto* object = static_cast<UObject*>(actor);
        if (object && object == m_controller) forget_player();
        else if (object && object == m_pawn)
        {
            g_player_camera.store(nullptr);
            g_player_root.store(nullptr);
            m_pawn = nullptr;
        }
    }

    // Game thread, every tick: one pointer read unless the pawn changed.
    auto on_engine_tick() -> void
    {
        if (!m_controller && m_find_requested.exchange(false))
        {
            m_controller = UObjectGlobals::FindFirstOf(STR("BP_PlayerController_C"));
            Output::send<LogLevel::Normal>(STR("[DWSmoothCam] player controller {}\n"), m_controller ? STR("found") : STR("not found"));
        }
        if (!m_controller) return;

        // Name lookup once per controller class, then a plain read.
        if (m_pawn_offset < 0)
        {
            auto** slot = m_controller->GetValuePtrByPropertyNameInChain<UObject*>(STR("Pawn"));
            if (!slot)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothCam] controller has no Pawn property\n"));
                m_controller = nullptr;
                return;
            }
            m_pawn_offset = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(m_controller));
        }
        auto* pawn = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(m_controller) + m_pawn_offset);
        if (pawn == m_pawn) return;
        m_pawn = pawn;
        g_player_camera.store(nullptr);
        g_player_root.store(nullptr);
        g_reset.store(true);
        if (!pawn) return;

        auto* camera = object_ptr(pawn, STR("FollowCamera"));
        auto* root = object_ptr(pawn, STR("RootComponent"));
        if (!camera || !root)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] pawn {} has no FollowCamera or RootComponent\n"), pawn->GetName());
            return;
        }
        if (g_translation_offset.load() < 0 && !find_translation_offset(root)) return;

        g_player_root.store(root);
        g_player_camera.store(camera);
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] following {}\n"), pawn->GetName());
    }

    // ComponentToWorld is not reflected. A root component has no parent, so its world translation equals
    // its reflected RelativeLocation: find the one other place those three doubles sit, followed by a
    // (1, 1, 1) scale 0x20 further on (UE 5.5 FTransform: rotation, translation, scale, 0x20 each).
    auto find_translation_offset(UObject* root) -> bool
    {
        auto* relative = root->GetValuePtrByPropertyNameInChain<double>(STR("RelativeLocation"));
        if (!relative)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] RelativeLocation not found, smoothing inactive\n"));
            return false;
        }
        auto base = reinterpret_cast<uint8_t*>(root);
        auto relative_offset = reinterpret_cast<uint8_t*>(relative) - base;
        double want[3]{relative[0], relative[1], relative[2]};

        int32_t found = -1;
        int matches = 0;
        for (int32_t offset = 0x28; offset + 0x40 <= 0x800; offset += 8)
        {
            if (offset == relative_offset) continue;
            double block[7]{};
            if (!guarded_read(base + offset, block, sizeof(block))) break;
            bool location = std::abs(block[0] - want[0]) < 0.5 && std::abs(block[1] - want[1]) < 0.5 && std::abs(block[2] - want[2]) < 0.5;
            bool scale = std::abs(block[4] - 1.0) < 1e-3 && std::abs(block[5] - 1.0) < 1e-3 && std::abs(block[6] - 1.0) < 1e-3;
            if (location && scale)
            {
                found = offset;
                ++matches;
            }
        }
        if (matches != 1)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] ComponentToWorld translation: {} matches, smoothing inactive\n"), matches);
            return false;
        }
        g_translation_offset.store(found);
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] ComponentToWorld translation at 0x{:X} (RelativeLocation 0x{:X})\n"), found,
                                       relative_offset);
        return true;
    }
};

#define DW_SMOOTHCAM_API __declspec(dllexport)
extern "C"
{
    DW_SMOOTHCAM_API CppUserModBase* start_mod()
    {
        return new DWSmoothCam();
    }

    DW_SMOOTHCAM_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
