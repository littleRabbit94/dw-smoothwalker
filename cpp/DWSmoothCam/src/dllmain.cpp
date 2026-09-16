// DWSmoothCam: frame-interpolated third-person camera for The Blood of Dawnwalker.
// Hooks RebelCameraComponent::GetCameraView (vtable slot 214), lets the game build its view, then lags
// the character pivot the view is built around and adds that lag to the camera. The game's collision,
// offsets, FOV and blends stay the game's.
// Design notes: docs/design.md "DWSmoothCam"; docs/design.md "The gameplay camera", "The per-frame hook".
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "config.hpp"
#include "mode_tuning.hpp"
#include "smoothing.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace
{
    constexpr size_t GET_CAMERA_VIEW_SLOT = 214;
    constexpr const char* SETTINGS_PATH = "ue4ss/Mods/DWSmoothCam/scripts/config/smoothcam.ini";
    constexpr const char* PRESETS_PATH = "ue4ss/Mods/DWSmoothCam/scripts/config/presets.ini";

    // Leading members of UE 5.5 FMinimalViewInfo: Location, Rotation (Pitch, Yaw, Roll).
    struct ViewHead
    {
        double location[3];
        double rotation[3];
    };

    // What the hook needs, numbers only: copying it allocates nothing on the worker thread.
    struct Tuning
    {
        double follow_rate_h, follow_rate_v;
        int curve_h, curve_v;
        double catchup_distance, min_rate_scale, max_lag_h, max_lag_v;
        bool soft_leash, rotation_smoothing, wall_clamp;
        double rotation_rate, reset_distance, reset_gap;
    };

    auto tuning_of(const dwsc::Settings& s) -> Tuning
    {
        return {s.follow_rate_h, s.follow_rate_v, s.curve_h, s.curve_v, s.catchup_distance, s.min_rate_scale, s.max_lag_h, s.max_lag_v,
                s.soft_leash, s.rotation_smoothing, s.wall_clamp, s.rotation_rate, s.reset_distance, s.reset_gap};
    }

    using GetCameraViewFn = void(__fastcall*)(void* self, float delta_time, void* desired_view);

    GetCameraViewFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;

    SRWLOCK g_tuning_lock = SRWLOCK_INIT;
    Tuning g_tuning = tuning_of(dwsc::Settings{});

    // Published by the game thread, read by the hook. Pointers are only compared or read under SEH.
    std::atomic<bool> g_enabled{true};
    std::atomic<bool> g_reset{true};
    std::atomic<bool> g_log_stats{false};
    std::atomic<void*> g_player_camera{nullptr};
    std::atomic<void*> g_player_root{nullptr};
    std::atomic<int32_t> g_translation_offset{-1}; // USceneComponent::ComponentToWorld.Translation

    // Stats for log_stats.
    std::atomic<uint64_t> g_frames{0};
    std::atomic<uint64_t> g_clamped{0};
    std::atomic<double> g_lag_sum{0.0};
    std::atomic<uint64_t> g_view_updates{0}; // GetCameraView calls for the player's camera: the camera is updating
    std::atomic<uint64_t> g_calls_timed{0}; // smooth_view calls, and QPC ticks spent in them
    std::atomic<uint64_t> g_ticks_spent{0};

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

    // Soft leash: the internal lag may run to three times the limit, and what the camera shows is eased
    // into the limit (tanh), so reaching it has no edge. Hard: shown lag stops at the limit.
    auto leash(double lag, double limit, bool soft) -> double
    {
        if (limit <= 0.0) return 0.0;
        return soft ? limit * std::tanh(lag / limit) : std::min(lag, limit);
    }

    auto smooth_view(void* desired_view, float delta_time) -> void
    {
        AcquireSRWLockShared(&g_tuning_lock);
        const Tuning t = g_tuning;
        ReleaseSRWLockShared(&g_tuning_lock);

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

        // Snap after a toggle, a settings change, a gap (cutscene, photo mode, load) or a teleport.
        bool snap = g_reset.exchange(false, std::memory_order_relaxed) || !g_follow.valid ||
                    seconds_between(g_follow.last_call, now) > t.reset_gap || dwsc::length(pivot - g_follow.pivot_last) > t.reset_distance;
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

        // Horizontal follow on the internal pivot, bounded at the internal leash.
        dwsc::Vec3& ps = g_follow.pivot_smoothed;
        double inner_h = t.soft_leash ? 3.0 * t.max_lag_h : t.max_lag_h;
        double inner_v = t.soft_leash ? 3.0 * t.max_lag_v : t.max_lag_v;

        double lag_hx = pivot.x - ps.x, lag_hy = pivot.y - ps.y;
        double lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
        double a_h = dwsc::follow_alpha(t.follow_rate_h, t.curve_h, lag_h, t.catchup_distance, t.min_rate_scale, dt);
        ps.x += lag_hx * a_h;
        ps.y += lag_hy * a_h;
        lag_hx = pivot.x - ps.x;
        lag_hy = pivot.y - ps.y;
        lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
        if (lag_h > inner_h && lag_h > 0.0)
        {
            double k = inner_h / lag_h;
            ps.x = pivot.x - lag_hx * k;
            ps.y = pivot.y - lag_hy * k;
            lag_hx *= k;
            lag_hy *= k;
            lag_h = inner_h;
        }

        double lag_v = pivot.z - ps.z;
        double a_v = dwsc::follow_alpha(t.follow_rate_v, t.curve_v, std::abs(lag_v), t.catchup_distance, t.min_rate_scale, dt);
        ps.z += lag_v * a_v;
        lag_v = pivot.z - ps.z;
        if (std::abs(lag_v) > inner_v)
        {
            lag_v = std::copysign(inner_v, lag_v);
            ps.z = pivot.z - lag_v;
        }

        // The lag the camera shows.
        double shown_h = leash(lag_h, t.max_lag_h, t.soft_leash);
        double scale_h = lag_h > 0.0 ? shown_h / lag_h : 0.0;
        double shown_v = std::copysign(leash(std::abs(lag_v), t.max_lag_v, t.soft_leash), lag_v);
        dwsc::Vec3 shown_pivot{pivot.x - lag_hx * scale_h, pivot.y - lag_hy * scale_h, pivot.z - shown_v};

        // Rotation: slerp toward the game's rotation, and swing the camera around the pivot to match.
        dwsc::Vec3 arm = camera - pivot;
        if (t.rotation_smoothing)
        {
            double a_r = 1.0 - std::exp(-std::max(t.rotation_rate, 0.0) * dt);
            g_follow.rotation_smoothed = dwsc::slerp(g_follow.rotation_smoothed, rotation, a_r);
            dwsc::Quat delta = dwsc::multiply(g_follow.rotation_smoothed, dwsc::conjugate(rotation));
            arm = dwsc::rotate(delta, arm);
            dwsc::to_rotator(g_follow.rotation_smoothed, view.rotation[0], view.rotation[1], view.rotation[2]);
        }
        else
        {
            g_follow.rotation_smoothed = rotation;
        }

        dwsc::Vec3 result = shown_pivot + arm;

        // Walls: the game already pulled its camera in front of any wall. While the camera is closer to
        // the pivot than it has recently been, keep the smoothed camera no farther out than the game's.
        double game_distance = dwsc::length(arm);
        double settle = 1.0 - std::exp(-1.0 * dt);
        g_follow.nominal_distance = std::max(game_distance, g_follow.nominal_distance + (game_distance - g_follow.nominal_distance) * settle);
        if (t.wall_clamp && game_distance < 0.85 * g_follow.nominal_distance)
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

        if (g_log_stats.load(std::memory_order_relaxed))
        {
            g_frames.fetch_add(1, std::memory_order_relaxed);
            g_lag_sum.store(g_lag_sum.load(std::memory_order_relaxed) + dwsc::length(pivot - shown_pivot), std::memory_order_relaxed);
        }
    }

    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view)
    {
        g_original(self, delta_time, desired_view);
        if (self != g_player_camera.load(std::memory_order_relaxed)) return;
        g_view_updates.fetch_add(1, std::memory_order_relaxed);
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        if (!g_log_stats.load(std::memory_order_relaxed))
        {
            smooth_view(desired_view, delta_time);
            return;
        }
        LARGE_INTEGER start{}, stop{};
        QueryPerformanceCounter(&start);
        smooth_view(desired_view, delta_time);
        QueryPerformanceCounter(&stop);
        g_calls_timed.fetch_add(1, std::memory_order_relaxed);
        g_ticks_spent.fetch_add(static_cast<uint64_t>(stop.QuadPart - start.QuadPart), std::memory_order_relaxed);
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

    auto widen(const std::string& s) -> std::wstring
    {
        return std::wstring(s.begin(), s.end());
    }

    auto last_write(const char* path) -> uint64_t
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
        return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }
} // namespace

class DWSmoothCam : public CppUserModBase
{
  public:
    DWSmoothCam() : CppUserModBase()
    {
        ModName = STR("DWSmoothCam");
        ModVersion = STR("0.7.1");
        ModDescription = STR("Frame-interpolated third-person camera");
        ModAuthors = STR("littleRabbit6");

        QueryPerformanceFrequency(&g_qpc_frequency);
        std::lock_guard guard(m_file_mutex);
        reload_settings_locked(true);
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] v{} loaded, {}\n"), ModVersion, g_enabled.load() ? STR("on") : STR("off"));
    }

    ~DWSmoothCam() override
    {
        m_tuner.restore();
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
        m_callbacks.push_back(Hook::RegisterBeginPlayPostCallback([this](auto&, AActor* actor) { on_begin_play(actor); }, options));
        m_callbacks.push_back(Hook::RegisterEndPlayPostCallback([this](auto&, AActor* actor, EEndPlayReason) { on_end_play(actor); }, options));
        m_callbacks.push_back(Hook::RegisterLoadMapPreCallback(
                [this](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) {
                    forget_player();
                    m_tuner.forget();
                    m_position_applied_generation = 0; // re-apply in the next world
                }, options));
        m_callbacks.push_back(Hook::RegisterEngineTickPostCallback([this](auto&, UEngine*, float, bool) { on_engine_tick(); }, options));

        bind(m_toggle_key, STR("toggle_key"), [this]() {
            bool now = !g_enabled.load();
            g_reset.store(true);
            g_enabled.store(now);
            if (!m_controller) m_find_requested.store(true); // e.g. after a hot reload mid-game
            Output::send<LogLevel::Normal>(STR("[DWSmoothCam] smoothing {}\n"), now ? STR("on") : STR("off"));
            request_banner(now ? STR("SmoothCam: On") : STR("SmoothCam: Off"));
        });
        bind(m_preset_key, STR("preset_key"), [this]() { cycle_preset(); });
        bind(m_shoulder_key, STR("shoulder_key"), [this]() { swap_shoulder(); });

        m_last_report = m_last_poll = std::chrono::steady_clock::now();
    }

    auto on_update() -> void override
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - m_last_poll).count() >= 0.25)
        {
            m_last_poll = now;
            std::lock_guard guard(m_file_mutex);
            if (last_write(SETTINGS_PATH) != m_settings_stamp) reload_settings_locked(false);
        }

        if (!g_log_stats.load()) return;
        auto elapsed = std::chrono::duration<double>(now - m_last_report).count();
        if (elapsed < 5.0) return;
        m_last_report = now;
        auto frames = g_frames.exchange(0);
        auto clamped = g_clamped.exchange(0);
        auto lag = g_lag_sum.exchange(0.0);
        auto timed = g_calls_timed.exchange(0);
        auto ticks = g_ticks_spent.exchange(0);
        double micros = timed ? 1e6 * static_cast<double>(ticks) / static_cast<double>(g_qpc_frequency.QuadPart) / timed : 0.0;
        Output::send<LogLevel::Normal>(
                STR("[DWSmoothCam] {:.1f} smoothed frames/s, mean shown lag {:.1f} cm, wall clamp {:.0f}%, {:.1f} us per frame in the hook\n"),
                frames / elapsed, frames ? lag / frames : 0.0, frames ? 100.0 * clamped / frames : 0.0, micros);
    }

  private:
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    FName m_player_controller_name{};
    UObject* m_controller = nullptr; // game thread only
    UObject* m_pawn = nullptr;
    int32_t m_pawn_offset = -1; // AController::Pawn, same class every map
    std::atomic<bool> m_find_requested{false};

    std::mutex m_file_mutex;     // settings file, presets file and m_settings: UE4SS thread and key callbacks
    dwsc::Settings m_settings{};
    uint64_t m_settings_stamp = 0;
    std::string m_toggle_key, m_preset_key, m_shoulder_key;

    // Camera position: settings published under m_position_mutex, applied from the engine tick.
    dwsc::ModeTuner m_tuner; // game thread only
    std::mutex m_position_mutex;
    dwsc::PositionTuning m_position{};
    std::atomic<uint64_t> m_position_generation{1};
    uint64_t m_position_applied_generation = 0; // game thread only
    size_t m_preset_cursor = 0;

    std::chrono::steady_clock::time_point m_last_report{}, m_last_poll{};

    // Banner: requested from any thread, shown from the engine tick.
    std::mutex m_banner_mutex;
    std::wstring m_banner_text;
    std::atomic<bool> m_banner_pending{false};
    std::atomic<bool> m_show_banner{true};
    int m_banner_state = 0; // 0 unresolved, 1 ready, -1 unavailable (game thread only)
    UFunction* m_banner_function = nullptr;
    UObject* m_banner_library = nullptr;

    auto request_banner(std::wstring text) -> void
    {
        if (!m_show_banner.load()) return;
        {
            std::lock_guard guard(m_banner_mutex);
            m_banner_text = std::move(text);
        }
        m_banner_pending.store(true);
    }

    // PushRegionEnteredNotification(WorldContextObject, RegionData, IsNewlyDiscovered): the region banner,
    // with RegionData.RegionDisplayText as the line shown. The parameter layout is checked against
    // reflection once; any difference switches banners off rather than guessing.
    auto resolve_banner() -> bool
    {
        m_banner_state = -1;
        m_banner_function = UObjectGlobals::StaticFindObject<UFunction*>(
                nullptr, nullptr, STR("/Script/DogwoodUI.NotificationSystemLibrary:PushRegionEnteredNotification"));
        m_banner_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/DogwoodUI.Default__NotificationSystemLibrary"));
        auto* region = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/DogwoodSystem.RegionData"));
        if (!m_banner_function || !m_banner_library || !region)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] notification function not found, banners off\n"));
            return false;
        }

        auto offset_of = [](UStruct* owner, const wchar_t* name) -> int32_t {
            for (FProperty* property : owner->ForEachProperty())
            {
                if (property->GetName() == name) return property->GetOffset_ForInternal();
            }
            return -1;
        };
        auto world = offset_of(m_banner_function, STR("WorldContextObject"));
        auto data = offset_of(m_banner_function, STR("RegionData"));
        auto flag = offset_of(m_banner_function, STR("IsNewlyDiscovered"));
        auto text = offset_of(region, STR("RegionDisplayText"));
        if (world != BANNER_WORLD || data != BANNER_DATA || flag != BANNER_FLAG || text != BANNER_TEXT)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] notification layout changed ({}, {}, {}, {}), banners off\n"), world, data, flag, text);
            return false;
        }
        m_banner_state = 1;
        return true;
    }

    auto show_pending_banner() -> void
    {
        if (!m_banner_pending.load() || !m_controller) return;
        if (m_banner_state == 0) resolve_banner();
        m_banner_pending.store(false);
        if (m_banner_state != 1) return;

        std::wstring line;
        {
            std::lock_guard guard(m_banner_mutex);
            line = m_banner_text;
        }
        FText text(line.c_str());
        uint8_t params[BANNER_PARAMS_SIZE]{};
        memcpy(params + BANNER_WORLD, &m_controller, sizeof(m_controller));
        text.CopyBorrowedTo(params + BANNER_DATA + BANNER_TEXT);
        params[BANNER_FLAG] = 0; // not newly discovered: no discovery reward
        m_banner_library->ProcessEvent(m_banner_function, params);
    }

    static constexpr int32_t BANNER_WORLD = 0x00;
    static constexpr int32_t BANNER_DATA = 0x08;
    static constexpr int32_t BANNER_TEXT = 0x20; // inside RegionData
    static constexpr int32_t BANNER_FLAG = 0x40;
    static constexpr size_t BANNER_PARAMS_SIZE = 0x48;

    auto bind(const std::string& name, const TCHAR* setting, std::function<void()> action) -> void
    {
        if (name.empty()) return;
        int key = parse_key(name);
        if (key < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothCam] unknown {} '{}', not bound\n"), setting, widen(name));
            return;
        }
        register_keydown_event(static_cast<Input::Key>(key), std::move(action));
    }

    auto publish_locked() -> void
    {
        AcquireSRWLockExclusive(&g_tuning_lock);
        g_tuning = tuning_of(m_settings);
        ReleaseSRWLockExclusive(&g_tuning_lock);
        g_log_stats.store(m_settings.log_stats);
        m_show_banner.store(m_settings.show_banner);
        g_reset.store(true);

        auto position = dwsc::position_of(m_settings);
        std::lock_guard guard(m_position_mutex);
        if (!(position == m_position))
        {
            m_position = position;
            m_position_generation.fetch_add(1);
        }
    }

    // Flips which shoulder the camera sits over, in every mode that has a side, and saves it to the ini.
    auto swap_shoulder() -> void
    {
        std::lock_guard guard(m_file_mutex);
        m_settings.shoulder_swap = !m_settings.shoulder_swap;
        if (auto content = dwsc::read_file(SETTINGS_PATH))
        {
            dwsc::Values values{{"shoulder_swap", m_settings.shoulder_swap ? 1.0 : 0.0}};
            if (dwsc::write_file(SETTINGS_PATH, dwsc::rewrite_numbers(*content, values))) m_settings_stamp = last_write(SETTINGS_PATH);
        }
        publish_locked();
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] shoulder {}\n"), m_settings.shoulder_swap ? STR("swapped") : STR("as the game has it"));
        request_banner(m_settings.shoulder_swap ? STR("SmoothCam: Shoulder swapped") : STR("SmoothCam: Shoulder default"));
    }

    // Game thread: applies camera position once a player camera exists and the settings moved.
    auto apply_position() -> void
    {
        m_tuner.tick(g_view_updates.load());
        auto* camera = static_cast<UObject*>(g_player_camera.load());
        if (!camera) return;
        auto generation = m_position_generation.load();
        if (generation == m_position_applied_generation) return;
        dwsc::PositionTuning position;
        {
            std::lock_guard guard(m_position_mutex);
            position = m_position;
        }
        m_tuner.apply(position, camera);
        m_position_applied_generation = generation;
    }

    // Reads smoothcam.ini, runs a pending menu save or load, writes the file back if either ran, publishes.
    auto reload_settings_locked(bool startup) -> void
    {
        auto content = dwsc::read_file(SETTINGS_PATH);
        m_settings_stamp = last_write(SETTINGS_PATH);
        if (!content)
        {
            if (startup) Output::send<LogLevel::Warning>(STR("[DWSmoothCam] smoothcam.ini not found, using defaults\n"));
            m_settings = dwsc::Settings{};
            publish_locked();
            return;
        }

        auto previous_enabled = m_settings.enabled;
        m_settings = dwsc::parse_settings(*content);
        if (startup)
        {
            m_toggle_key = m_settings.toggle_key;
            m_preset_key = m_settings.preset_key;
            m_shoulder_key = m_settings.shoulder_key;
        }

        dwsc::Values writes;
        if (m_settings.preset_save >= 1 && m_settings.preset_save <= 6)
        {
            auto slots = dwsc::read_slots(PRESETS_PATH);
            slots[m_settings.preset_save] = dwsc::preset_of(m_settings);
            bool ok = dwsc::write_slots(PRESETS_PATH, slots);
            Output::send<LogLevel::Normal>(STR("[DWSmoothCam] saved slot {}{}\n"), m_settings.preset_save, ok ? STR("") : STR(": write failed"));
            if (ok) request_banner(std::format(STR("SmoothCam: saved to Slot {}"), m_settings.preset_save));
        }
        if (m_settings.preset_save != 0) writes.emplace_back("preset_save", 0);

        if (m_settings.preset_load != 0)
        {
            if (auto preset = find_preset(m_settings.preset_load))
            {
                dwsc::apply_values(m_settings, preset->second);
                writes.insert(writes.end(), preset->second.begin(), preset->second.end());
                Output::send<LogLevel::Normal>(STR("[DWSmoothCam] loaded preset {}\n"), widen(preset->first));
                request_banner(STR("SmoothCam: ") + widen(preset->first));
            }
            else
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothCam] preset {} is empty, nothing loaded\n"), m_settings.preset_load);
            }
            writes.emplace_back("preset_load", 0);
        }
        m_settings.preset_save = m_settings.preset_load = 0;

        if (!writes.empty())
        {
            if (dwsc::write_file(SETTINGS_PATH, dwsc::rewrite_numbers(*content, writes))) m_settings_stamp = last_write(SETTINGS_PATH);
            else Output::send<LogLevel::Warning>(STR("[DWSmoothCam] could not write smoothcam.ini\n"));
        }

        publish_locked();
        if (startup || m_settings.enabled != previous_enabled) g_enabled.store(m_settings.enabled);
        if (!startup) Output::send<LogLevel::Normal>(STR("[DWSmoothCam] settings applied\n"));
    }

    // 101-103 built-in, 1-6 user slot. Returns its name and values.
    auto find_preset(int id) -> std::optional<std::pair<std::string, dwsc::Values>>
    {
        for (auto& p : dwsc::builtin_presets())
        {
            if (p.id == id) return std::make_pair(std::string(p.name), p.values);
        }
        if (id >= 1 && id <= 6)
        {
            auto slots = dwsc::read_slots(PRESETS_PATH);
            if (auto it = slots.find(id); it != slots.end() && !it->second.empty())
            {
                return std::make_pair("Slot " + std::to_string(id), it->second);
            }
        }
        return std::nullopt;
    }

    // Built-ins, then saved slots. Writes the values into smoothcam.ini so the menu shows them.
    auto cycle_preset() -> void
    {
        std::lock_guard guard(m_file_mutex);
        std::vector<int> order;
        for (auto& p : dwsc::builtin_presets()) order.push_back(p.id);
        for (auto& [slot, values] : dwsc::read_slots(PRESETS_PATH))
        {
            if (!values.empty()) order.push_back(slot);
        }
        auto preset = find_preset(order[m_preset_cursor++ % order.size()]);
        if (!preset) return;

        auto content = dwsc::read_file(SETTINGS_PATH);
        dwsc::apply_values(m_settings, preset->second);
        if (content && dwsc::write_file(SETTINGS_PATH, dwsc::rewrite_numbers(*content, preset->second)))
        {
            m_settings_stamp = last_write(SETTINGS_PATH);
        }
        publish_locked();
        Output::send<LogLevel::Normal>(STR("[DWSmoothCam] preset {}\n"), widen(preset->first));
        request_banner(STR("SmoothCam: ") + widen(preset->first));
    }

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
        show_pending_banner();
        apply_position();
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
        m_position_applied_generation = 0; // a new pawn: its modes get the current position
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
