// Smoothwalker's side of the DLL (docs/design.md, "Core and processors"): the follow as the camera core's processor
// (follow/processor.hpp), camera position in the game's modes (modes/mode_tuner.hpp), presets and slots,
// smoothwalker.ini, keys, banners and the debug overlay (ui/debug_overlay.hpp). Reaches the core only through its
// interface (camera/api.hpp).
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "follow/processor.hpp"
#include "smoothwalker.hpp"
#include "settings/ini.hpp"
#include "settings/presets.hpp"
#include "settings/settings.hpp"
#include "ui/debug_overlay.hpp"
#include "modes/mode_tuner.hpp"
#include "../common/live_ref.hpp"
#include "../common/log.hpp"
#include "../common/math.hpp"
#include "../common/text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace dw::smoothwalker
{
namespace
{
    // Not under scripts/: UE4SS makes a Lua mod of any folder with a scripts subfolder and logs a missing main.lua.
    constexpr const char* SETTINGS_PATH = "ue4ss/Mods/DWSmoothwalker/config/smoothwalker.ini";
    constexpr const char* PENDING_PATH = "ue4ss/Mods/DWSmoothwalker/config/smoothwalker.pending";
    constexpr const char* PRESETS_DIR = "ue4ss/Mods/DWSmoothwalker/config/presets";
    constexpr const wchar_t* PRESETS_DIR_W = L"ue4ss\\Mods\\DWSmoothwalker\\config\\presets";
    constexpr const char* MANIFEST_PATH = "ue4ss/Mods/DWSmoothwalker/mod_settings.ini";

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

struct Smoothwalker::Impl
{
    camera::CameraCore& m_core;            // the camera core's interface: everything this side knows of the core
    BindKey m_bind_key;
    std::wstring m_version;
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    follow::FollowProcessor m_processor;   // registered with the core for this object's life

    std::mutex m_file_mutex; // the config files, m_settings, m_baseline, m_presets, m_loaded_id; the engine tick never takes it
    settings::Settings m_settings{};
    uint64_t m_settings_stamp = 0;                // write time of smoothwalker.ini as last read or written
    std::map<std::string, double> m_baseline;     // each numeric key as last known to be in smoothwalker.ini
    int m_loaded_id = 0;                          // last preset loaded or cycled; shown while it still matches
    UClass* m_activatable_class = nullptr;        // CommonActivatableWidget, the Mod Menu's host class
    UObject* m_menu_host = nullptr;               // the Mod Menu host last seen open; checked before any rescan
    bool m_menu_logged = false;
    bool m_custom_pinned = false;                 // Custom was picked: shown until a preset is loaded or a slot adopted
    std::vector<settings::Preset> m_presets;      // built-ins, slots, drop-ins, in cycle order; scanned once per session
    size_t m_loaded_slots = 0, m_loaded_dropins = 0; // load_presets_locked()'s counts, for the constructor's load line
    std::string m_pending_file;                   // content last written to smoothwalker.pending; empty when none
    std::atomic<bool> m_flush_pending{false};     // live settings differ from m_baseline
    bool m_flush_failing = false;                 // a write-back failed; warned once until one succeeds
    std::chrono::steady_clock::time_point m_next_flush{};
    std::string m_toggle_key, m_preset_key, m_shoulder_key, m_debug_key;

    modes::ModeTuner m_tuner; // game thread only
    std::mutex m_position_mutex;
    modes::PositionTuning m_position{};
    std::atomic<uint64_t> m_position_generation{1};
    uint64_t m_position_applied_generation = 0; // game thread only

    std::chrono::steady_clock::time_point m_last_report{}, m_last_poll{};

    std::mutex m_banner_mutex; // m_banner_text, m_banner_due, m_banner_pending
    std::wstring m_banner_text;
    std::chrono::steady_clock::time_point m_banner_due{};
    bool m_banner_pending = false;
    int32_t m_queue_offset = -1;        // NotificationSubsystem::NotificationQueue
    int32_t m_region_data_offset = -1;  // RegionEnteredNotificationInfo::RegionData
    UClass* m_region_info_class = nullptr;
    std::atomic<bool> m_show_banner{true};
    int m_banner_state = 0; // 0 unresolved, 1 ready, -1 unavailable (game thread only)
    UFunction* m_banner_function = nullptr;
    UObject* m_banner_library = nullptr;
    dw::LiveRef m_notifications; // NotificationSubsystem, game thread only, checked live before use

    ui::DebugOverlay m_overlay;            // game thread only
    std::atomic<bool> m_debug_overlay{false};
    std::mutex m_debug_mutex;              // m_debug_preset, m_debug_tuning: written by publish_locked, read by the overlay's refresh
    std::wstring m_debug_preset;           // the active preset's name, or Custom
    bool m_debug_tuning = true;            // camera_tuning

    // The core's game-thread notifications (camera::Listener), kept for this object's life. A member, so Impl itself
    // stays non-virtual.
    struct Hooks final : camera::Listener
    {
        Impl& self;
        explicit Hooks(Impl& impl) : self(impl) {}
        auto world_changed() -> void override { self.on_world_changed(); }
        auto camera_changed() -> void override { self.on_camera_changed(); }
        auto tick() -> void override { self.on_tick(); }
    };
    Hooks m_hooks{*this};

    Impl(camera::CameraCore& core, BindKey bind_key, std::wstring version) : m_core(core), m_bind_key(std::move(bind_key)), m_version(std::move(version))
    {
        if (!m_core.register_processor(m_processor))
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] the camera core already runs another view processor: smoothing inactive\n"));
        if (!m_core.set_listener(m_hooks))
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] the camera core already has a listener: camera position tuning inactive\n"));

        std::lock_guard guard(m_file_mutex);
        // log_verbose is read once here, ahead of load_presets_locked(), so its per-preset lines are gated from
        // the start; reload_settings_locked(true) below re-derives the same value from the full parse. Written
        // whatever the file holds (off without the key or the file): the flag is image-level (common/log.hpp), and
        // this write, not an unload reset, is what starts each instance from its own ini.
        bool verbose = false;
        if (auto content = settings::read_file(SETTINGS_PATH))
        {
            auto numbers = settings::parse_numbers(*content);
            if (auto found = numbers.find("log_verbose"); found != numbers.end()) verbose = found->second != 0.0;
        }
        dw::g_verbose.store(verbose);
        load_presets_locked();
        reload_settings_locked(true);
        add_missing_keys_locked();
        // Once, without the camera_live() gate: no Mod Menu page can be open this early (docs/design.md, "Startup
        // flush"). Fixes a preset id the regenerated manifest may not list yet, before the page can fail on it.
        if (m_flush_pending.load()) flush_locked(std::chrono::steady_clock::now());
        auto s = [](size_t n) { return n == 1 ? STR("") : STR("s"); };
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] v{} loaded, {}, {} saved slot{}, {} preset{} from the presets folder\n"), m_version,
                                       m_processor.enabled() ? STR("on") : STR("off"), m_loaded_slots, s(m_loaded_slots), m_loaded_dropins, s(m_loaded_dropins));
    }

    auto start() -> void
    {
        // Runs on whatever thread constructs the object (async loading threads too): only a flag test and a pointer
        // compare and, on a match, an index read and a locked hand-off (a mode pushed on the player's camera into
        // m_tuner). On the game thread a tracked mode is written here instead: the push that follows copies its FOV
        // (ModeTuner::write_new()).
        Hook::FCallbackOptions options{false, true, STR("DWSmoothwalker"), STR("")};
        auto id = Hook::RegisterStaticConstructObjectPostCallback(
                [this](auto& info, const FStaticConstructObjectParameters& params) {
                    if (static_cast<uint32_t>(params.SetFlags) & static_cast<uint32_t>(RF_ClassDefaultObject | RF_ArchetypeObject)) return;
                    auto* cls = const_cast<UClass*>(params.Class);
                    if (!cls) return;
                    auto* camera = m_core.player_camera();
                    if (!camera || params.Outer != camera) return;
                    if (auto* mode = info.GetCurrentResolvedReturnValue())
                        m_tuner.note_new(dw::LiveRef::of(mode), GetCurrentThreadId() == m_core.game_thread_id());
                },
                options);
        if (id != Hook::ERROR_ID) m_callbacks.push_back(id);

        bind(m_toggle_key, STR("toggle_key"), [this]() {
            std::lock_guard guard(m_file_mutex);
            bool now = !m_processor.enabled();
            set_enabled_locked(now);
            publish_locked();
            mark_pending_locked();
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] smoothing {}\n"), now ? STR("on") : STR("off"));
            request_banner(now ? STR("Smoothwalker: On") : STR("Smoothwalker: Off"));
        });
        bind(m_preset_key, STR("preset_key"), [this]() {
            if (key_live(STR("preset"))) cycle_preset();
        });
        bind(m_shoulder_key, STR("shoulder_key"), [this]() {
            if (key_live(STR("shoulder"))) swap_shoulder();
        });
        // Written back like the toggle key's change, so the Mod Menu page shows it. Live under a pause too: it moves
        // nothing in the game.
        bind(m_debug_key, STR("debug_key"), [this]() {
            std::lock_guard guard(m_file_mutex);
            m_settings.debug_overlay = !m_settings.debug_overlay;
            publish_locked();
            mark_pending_locked();
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] debug overlay {}\n"), m_settings.debug_overlay ? STR("on") : STR("off"));
        });

        m_last_report = m_last_poll = std::chrono::steady_clock::now();
    }

    auto update() -> void
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - m_last_poll).count() >= 0.25)
        {
            m_last_poll = now;
            std::lock_guard guard(m_file_mutex);
            if (last_write(SETTINGS_PATH) != m_settings_stamp) reload_settings_locked(false);
        }
        // After the poll, so a file change is applied before the live values are written over it. Written while the
        // camera is live, or paused with the Mod Menu closed: a page reopened from the pause menu then shows the
        // loaded values, and no page is open to refuse its next Apply over the write.
        if (m_flush_pending.load() && now >= m_next_flush && (m_core.camera_live() || !mod_menu_open()))
        {
            std::lock_guard guard(m_file_mutex);
            flush_locked(now);
        }

        if (!m_processor.log_stats()) return;
        auto elapsed = std::chrono::duration<double>(now - m_last_report).count();
        if (elapsed < 5.0) return;
        m_last_report = now;
        // The follow's half from the processor, the hook's cost from the core; one line, as before the split.
        auto stats = m_processor.take_stats();
        uint64_t frames = stats.frames;
        uint64_t clamped = stats.clamped;
        double lag = stats.lag_sum;
        uint64_t timed = 0;
        double micros = 0.0;
        m_core.take_hook_timing(timed, micros);
        Output::send<LogLevel::Normal>(
                STR("[DWSmoothwalker] {:.1f} smoothed frames/s, mean shown lag {:.1f} cm, wall clamp {:.0f}%, {:.1f} us per frame in the hook\n"),
                frames / elapsed, frames ? lag / frames : 0.0, frames ? 100.0 * clamped / frames : 0.0, micros);
    }

    // Game thread, from the core's LoadMap callback or engine tick: the player is already forgotten.
    auto on_world_changed() -> void
    {
        m_tuner.forget();
        m_position_applied_generation = 0; // re-apply in the next world
        m_overlay.forget();                // the level took the panel off the viewport; the next pawn gets a new one
    }

    // Game thread: a new pawn's camera.
    auto on_camera_changed() -> void
    {
        m_position_applied_generation = 0; // a new pawn: its modes get the current position
        m_tuner.camera_changed();
    }

    // Game thread, every engine tick, after the core checked and discovered the controller.
    auto on_tick() -> void
    {
        show_pending_banner();
        apply_position();
        auto* controller = m_core.player_controller();
        auto* camera = m_core.player_camera();
        m_overlay.tick(m_debug_overlay.load(), controller, camera, [&] { return ui::format_panel(debug_panel(camera)); });
    }

    // The camera API as the panel shows it (CameraCore::camera_owner).
    auto api_status() -> ui::ApiStatus
    {
        ui::ApiStatus s;
        camera::Owner owner = m_core.camera_owner();
        if (owner.mod.empty()) return s;
        s.owner = dw::to_wide(owner.mod);
        s.lease = owner.lease;
        return s;
    }

    // Debounced: a burst of presses shows one banner, with the last text.
    // Dropped without a player: a banner queued at the main menu would show minutes later, after a load.
    auto request_banner(std::wstring text) -> void
    {
        if (!m_show_banner.load() || !m_core.player_known()) return;
        std::lock_guard guard(m_banner_mutex);
        m_banner_text = std::move(text);
        m_banner_due = std::chrono::steady_clock::now() + BANNER_SETTLE;
        m_banner_pending = true;
    }

    static constexpr auto BANNER_SETTLE = std::chrono::milliseconds(400);

    struct ObjectArray
    {
        UObject** data;
        int32_t num;
        int32_t max;
    };

    // Thins our own waiting banners so presses cannot build a backlog. The banner on screen is left alone:
    // ending a notification its widget is showing crashed the game (docs/design.md, "Rapid banners queue").
    // Ours are recognised by class and text, never by a remembered address, which the game may reuse.
    auto drop_stale_banners(UObject* subsystem) -> void
    {
        if (!subsystem) return;
        if (m_queue_offset < 0 || m_region_data_offset < 0 || !m_region_info_class)
        {
            auto* slot = subsystem->GetValuePtrByPropertyNameInChain<void>(STR("NotificationQueue"));
            m_region_info_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/DogwoodUI.RegionEnteredNotificationInfo"));
            int32_t data = -1;
            if (m_region_info_class)
            {
                for (FProperty* property : m_region_info_class->ForEachProperty())
                {
                    if (property->GetName() == STR("RegionData")) data = property->GetOffset_ForInternal();
                }
            }
            if (!slot || data < 0) return;
            m_queue_offset = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(subsystem));
            m_region_data_offset = data;
        }
        auto* queue = reinterpret_cast<ObjectArray*>(reinterpret_cast<uint8_t*>(subsystem) + m_queue_offset);
        if (queue->num < 0 || queue->num > queue->max || queue->max > 4096 || (queue->num > 0 && !queue->data)) return;

        int32_t kept = 0;
        for (int32_t i = 0; i < queue->num; ++i)
        {
            auto* entry = queue->data[i];
            if (!is_our_banner(entry)) queue->data[kept++] = entry;
        }
        queue->num = kept;
    }

    auto is_our_banner(UObject* entry) -> bool
    {
        if (!entry || entry->GetClassPrivate() != m_region_info_class) return false;
        auto* text = reinterpret_cast<FText*>(reinterpret_cast<uint8_t*>(entry) + m_region_data_offset + BANNER_TEXT);
        return text->ToString().starts_with(BANNER_PREFIX);
    }

    static constexpr const wchar_t* BANNER_PREFIX = L"Smoothwalker:";

    // The region banner shows RegionData.RegionDisplayText. The hard-coded parameter layout must match
    // reflection, or banners stay off.
    auto resolve_banner() -> bool
    {
        m_banner_state = -1;
        m_banner_function = UObjectGlobals::StaticFindObject<UFunction*>(
                nullptr, nullptr, STR("/Script/DogwoodUI.NotificationSystemLibrary:PushRegionEnteredNotification"));
        m_banner_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/DogwoodUI.Default__NotificationSystemLibrary"));
        auto* region = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/DogwoodSystem.RegionData"));
        if (!m_banner_function || !m_banner_library || !region)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] notification function not found, banners off\n"));
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
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] notification layout changed ({}, {}, {}, {}), banners off\n"), world, data, flag, text);
            return false;
        }
        m_banner_state = 1;
        return true;
    }

    auto show_pending_banner() -> void
    {
        auto* controller = m_core.player_controller(); // checked live this tick
        if (!controller) return;
        std::wstring line;
        {
            std::lock_guard guard(m_banner_mutex);
            if (!m_banner_pending || std::chrono::steady_clock::now() < m_banner_due) return;
            m_banner_pending = false;
            line = m_banner_text;
        }
        if (m_banner_state == 0) resolve_banner();
        if (m_banner_state != 1) return;

        // Cached: FindFirstOf walks the whole object array (28 ms measured), and this runs mid-glide after a preset change.
        if (!m_notifications.alive()) m_notifications = dw::LiveRef::of(UObjectGlobals::FindFirstOf(STR("NotificationSubsystem")));
        drop_stale_banners(m_notifications.object);
        FText text(line.c_str());
        uint8_t params[BANNER_PARAMS_SIZE]{};
        memcpy(params + BANNER_WORLD, &controller, sizeof(controller));
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
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unknown {} '{}', not bound\n"), setting, widen(name));
            return;
        }
        m_bind_key(key, std::move(action));
    }

    // The preset and shoulder keys act only while the mod is on and the camera is live. Off is the game as shipped,
    // so nothing may move; under a pause the change would land in smoothwalker.ini behind an open Mod Menu page.
    auto key_live(const TCHAR* key) -> bool
    {
        const TCHAR* why = !m_processor.enabled() ? STR("Smoothwalker is off") : !m_core.camera_live() ? STR("the camera is paused") : nullptr;
        if (why) Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] {} key ignored while {}\n"), key, why);
        return !why;
    }

    // The live switch, kept in m_settings too so the write-back shows it on the Mod Menu page. A change fades
    // (FollowProcessor::set_enabled).
    auto set_enabled_locked(bool on) -> void
    {
        m_settings.enabled = on;
        m_processor.set_enabled(on);
    }

    auto publish_locked() -> void
    {
        // No hard cut: the core crossfades on the new generations instead of snapping the lag away mid-motion. Only a
        // changed value starts one: a shoulder swap and the switches change none, and a fade holds part of the old lag.
        follow::publish_view(m_processor, m_core, m_settings);
        m_show_banner.store(m_settings.show_banner);
        m_debug_overlay.store(m_settings.debug_overlay);
        {
            auto* active = m_settings.preset != 0 ? find_preset(m_settings.preset) : nullptr;
            std::lock_guard guard(m_debug_mutex);
            m_debug_preset = m_settings.preset == 0 ? STR("Custom") : active ? dw::to_wide(active->name) : STR("preset ") + std::to_wstring(m_settings.preset);
            m_debug_tuning = m_settings.camera_tuning;
        }

        // enabled off is the game as shipped, camera modes and its own lag included (position_of).
        auto position = modes::position_of(m_settings);
        std::lock_guard guard(m_position_mutex);
        // With camera_tuning off only the lag switch is written, so a changed position number applies nothing.
        // The values are kept, so switching it on applies the latest.
        auto written = [](modes::PositionTuning p) {
            if (p.active) return p;
            modes::PositionTuning lag_only;
            lag_only.active = false;
            lag_only.own_lag = p.own_lag;
            return lag_only;
        };
        if (!(written(position) == written(m_position))) m_position_generation.fetch_add(1);
        m_position = position;
    }

    // Written back to the ini by flush_locked, so the Mod Menu page shows it.
    auto swap_shoulder() -> void
    {
        std::lock_guard guard(m_file_mutex);
        m_settings.shoulder_swap = !m_settings.shoulder_swap;
        update_active_locked();
        publish_locked();
        mark_pending_locked();
        // No banner: the camera moving to the other shoulder is the feedback.
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] shoulder {}{}\n"), m_settings.shoulder_swap ? STR("swapped") : STR("as the game has it"),
                                       m_settings.camera_tuning ? STR("") : STR(" (camera_tuning is 0: shows once it is on)"));
    }

    // Game thread.
    auto apply_position() -> void
    {
        auto* camera = m_core.player_camera();
        m_tuner.tick(m_core.view_updates(), m_core.view_seconds(), camera);
        // off: no per-tick GetState calls either
        auto state = camera && m_processor.enabled() ? m_tuner.mode_state(camera) : modes::ModeState{};
        m_processor.set_aiming(state.aiming);
        if (state.combat != m_processor.swap_combat(state.combat) && dw::verbose())
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] combat camera {}\n"), state.combat ? STR("on") : STR("off"));
        if (state.traversal != m_processor.swap_traversal(state.traversal) && dw::verbose())
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] traversal camera {}\n"), state.traversal ? STR("on") : STR("off"));
        if (!camera) return;
        auto generation = m_position_generation.load();
        if (generation == m_position_applied_generation) return;
        modes::PositionTuning position;
        {
            std::lock_guard guard(m_position_mutex);
            position = m_position;
        }
        m_tuner.apply(position, camera);
        m_position_applied_generation = generation;
        m_processor.mode_written(); // mode FOV lands on the next camera update: crossfade it
    }

    // Startup parses every value; a later change applies only keys whose number moved since the file was last
    // seen, so a live key or preset change survives an Apply of the rest. Only flush_locked writes smoothwalker.ini.
    auto reload_settings_locked(bool startup) -> void
    {
        // Stamp first: a write landing between the two is then seen by the next poll.
        auto stamp = last_write(SETTINGS_PATH);
        auto content = settings::read_file(SETTINGS_PATH);
        // At startup the key names are read only once, so ride out a Mod Menu rename in progress.
        for (int i = 0; startup && !content && i < 10; ++i)
        {
            Sleep(20);
            stamp = last_write(SETTINGS_PATH);
            content = settings::read_file(SETTINGS_PATH);
        }
        if (!content)
        {
            // The Mod Menu replaces the file by rename, so it is briefly absent: keep what is live and retry.
            if (!startup) return;
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] smoothwalker.ini not found, using defaults\n"));
            m_settings = settings::Settings{};
            m_toggle_key = m_settings.toggle_key;
            m_preset_key = m_settings.preset_key;
            m_shoulder_key = m_settings.shoulder_key;
            m_debug_key = m_settings.debug_key;
            publish_locked();
            return;
        }
        m_settings_stamp = stamp;
        auto file = settings::parse_numbers(*content);

        if (startup)
        {
            m_settings = settings::parse_settings(*content);
            m_toggle_key = m_settings.toggle_key;
            m_preset_key = m_settings.preset_key;
            m_shoulder_key = m_settings.shoulder_key;
            m_debug_key = m_settings.debug_key;
            m_baseline = std::move(file);
            apply_pending_file_locked();
            // Not a load request: only which of several matching presets to show.
            m_loaded_id = m_settings.preset;
            bool custom = m_settings.preset == 0;
            update_active_locked();
            // Custom in the file over values a preset matches: Custom was picked, so it stays (and saves into no slot).
            if (custom && m_settings.preset != 0)
            {
                m_custom_pinned = true;
                m_settings.preset = 0;
            }
            publish_locked();
            m_processor.store_enabled(m_settings.enabled);
            mark_pending_locked();
            return;
        }

        settings::Values edits;
        for (auto& [key, value] : file)
        {
            auto known = m_baseline.find(key);
            if (known == m_baseline.end() || known->second != value) edits.emplace_back(key, value);
            m_baseline[key] = value;
        }
        if (edits.empty())
        {
            mark_pending_locked(); // numbers unchanged; the stamp still moved
            return;
        }
        auto edited = [&](const char* key) {
            return std::any_of(edits.begin(), edits.end(), [&](auto& edit) { return edit.first == key; });
        };
        auto is_slot = [](int id) { return id >= 1 && id <= settings::MAX_SLOTS; };
        int active_before = m_settings.preset; // the slot an Apply that leaves the picker alone saves into

        // (a) Ordinary edits onto the live settings. The toggle's state changes only if the file's enabled did.
        settings::apply_values(m_settings, edits);
        if (edited("enabled")) set_enabled_locked(m_settings.enabled); // fades like the toggle key

        settings::Values own; // the preset keys this Apply moved
        for (auto& edit : edits)
        {
            if (settings::is_preset_key(edit.first)) own.push_back(edit);
        }
        auto slot_name = [&](int id) {
            auto* slot = find_preset(id);
            return dw::to_wide(slot ? slot->name : "Slot " + std::to_string(id));
        };
        // The slot file is written now: the menu does not watch it.
        auto save_slot = [&](int id) {
            bool ok = save_slot_locked(id, settings::preset_of(m_settings));
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] saved slot {}{}\n"), id, ok ? STR("") : STR(": write failed"));
            return ok;
        };

        int picked = m_settings.preset;
        if (edited("preset") && is_slot(picked) && !find_preset(picked))
        {
            // (b) An empty slot adopts the live values, this Apply's slider edits included. Not derived: the
            // menu already wrote preset = N, so there is nothing to flush at once.
            if (save_slot(picked))
            {
                m_loaded_id = picked;
                m_custom_pinned = false;
                request_banner(STR("Smoothwalker: saved to ") + slot_name(picked));
            }
        }
        else if (edited("preset") && picked != 0)
        {
            // (c) A changed picker loads that preset. Preset keys edited in the same Apply win over it, and on a
            // slot they go straight back into it. The live numbers now differ from what the Apply wrote; on_update
            // writes them once the menu closes (the open page would refuse its next Apply over the write), so a page
            // reopened from the pause menu shows the loaded values. Until then the open page's sliders are stale;
            // an Apply there still works and moves only the keys it changed.
            if (load_preset_locked(picked))
            {
                settings::apply_values(m_settings, own);
                if (is_slot(picked) && !own.empty()) save_slot(picked);
            }
        }
        else if (edited("preset"))
        {
            // Custom: detached from whatever was active, nothing saved. Pinned, or the values would match the slot
            // again and the next edit would be saved into it.
            m_loaded_id = 0;
            m_custom_pinned = true;
        }
        else if (is_slot(active_before) && !own.empty())
        {
            // (d) An edit with a slot active is saved into it. Built-ins and drop-ins stay read-only: an edit
            // there just falls through to update_active_locked, which gives Custom.
            if (save_slot(active_before))
            {
                m_loaded_id = active_before;
                request_banner(STR("Smoothwalker: ") + slot_name(active_before) + STR(" updated"));
            }
        }

        update_active_locked();
        publish_locked();
        mark_pending_locked();
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] settings applied\n"));
    }

    // Once per session, in the constructor, before anything reads presets or the Mod Menu reads mod_settings.ini:
    // built-ins, slots 1..MAX_SLOTS ("Slot N.ini"), then other presets-folder *.ini as drop-ins (201+, by name).
    // On a failed manifest rewrite drop-ins stay off, so the indicator never shows an id the page lacks.
    auto load_presets_locked() -> void
    {
        m_presets.clear();
        for (auto& p : settings::builtin_presets()) m_presets.push_back({p.id, p.name, p.values});
        CreateDirectoryW(PRESETS_DIR_W, nullptr); // so it is there to drop files into; fails harmlessly if present

        auto files = settings::list_preset_files(PRESETS_DIR_W);
        auto read_preset = [&](const std::wstring& name) -> std::optional<std::pair<std::string, settings::Values>> {
            auto content = settings::read_small_file(std::wstring(PRESETS_DIR_W) + L"\\" + name, settings::MAX_PRESET_FILE);
            if (!content)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] presets/{}: unreadable or over 64 KiB, skipped\n"), name);
                return std::nullopt;
            }
            auto parsed = settings::parse_preset_file(std::move(*content));
            if (parsed.second.empty())
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] presets/{}: no preset settings, skipped\n"), name);
                return std::nullopt;
            }
            return parsed;
        };

        for (auto& [slot, name] : files.slots)
        {
            // The file's name line is the slot's display name; the file name stays "Slot N.ini".
            if (auto parsed = read_preset(name))
            {
                m_presets.push_back({slot, settings::display_name(parsed->first, "Slot " + std::to_string(slot), slot), settings::normalize_preset(parsed->second)});
            }
        }

        std::vector<settings::Preset> dropins;
        size_t over_limit = 0;
        for (auto& name : files.dropins)
        {
            if (dropins.size() >= static_cast<size_t>(settings::MAX_DROPINS))
            {
                ++over_limit;
                continue;
            }
            auto parsed = read_preset(name);
            if (!parsed) continue;
            int id = settings::FIRST_DROPIN_ID + static_cast<int>(dropins.size());
            auto stem = dw::utf8_of(name.substr(0, name.size() - 4)).value_or("");
            dropins.push_back({id, settings::display_name(parsed->first, stem, id), settings::normalize_preset(parsed->second)});
        }
        if (over_limit)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {} presets over the limit of {} skipped\n"), over_limit, settings::MAX_DROPINS);
        }

        // Picker order: saved slots, drop-ins, then the empty slots, so nothing empty sits between the presets that
        // load. Empty slots stay listed: the page fails to open if the ini's preset id is not among the values, and
        // a slot saved this session becomes the active id.
        std::string values = "0|101|102|103", labels = "Custom|Tight|Balanced|Cinematic";
        std::string empty_values, empty_labels;
        for (int n = 1; n <= settings::MAX_SLOTS; ++n)
        {
            auto id = std::to_string(n);
            auto* saved = find_preset(n); // m_presets holds the built-ins and the slots here
            if (saved)
            {
                values += "|" + id;
                labels += "|" + saved->name;
            }
            else
            {
                empty_values += "|" + id;
                empty_labels += "|Slot " + id + " (empty)";
            }
        }
        for (auto& p : dropins)
        {
            values += "|" + std::to_string(p.id);
            labels += "|" + p.name;
        }
        values += empty_values;
        labels += empty_labels;
        bool listed = false;
        if (auto manifest = settings::read_file(MANIFEST_PATH))
        {
            if (auto updated = settings::with_preset_choices(*manifest, "[Setting.preset]", values, labels))
            {
                listed = *updated == *manifest || settings::write_file(MANIFEST_PATH, *updated);
                if (!listed) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not write mod_settings.ini\n"));
            }
            else
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] mod_settings.ini: [Setting.preset] PresetValues or PresetLabels missing\n"));
            }
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] mod_settings.ini not found\n"));
        }
        if (!listed && !dropins.empty())
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {} presets from the presets folder off for this session: the Mod Menu page could not list them\n"),
                                            dropins.size());
            dropins.clear();
        }
        bool verbose = dw::verbose();
        if (verbose)
        {
            for (auto& p : dropins)
            {
                Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] preset {} from the presets folder: {}\n"), p.id, dw::to_wide(p.name));
            }
        }
        size_t slots = m_presets.size() - settings::builtin_presets().size();
        m_presets.insert(m_presets.end(), std::make_move_iterator(dropins.begin()), std::make_move_iterator(dropins.end()));
        m_loaded_slots = slots;
        m_loaded_dropins = m_presets.size() - slots - settings::builtin_presets().size();
        if (verbose)
        {
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] presets: {} saved slots, {} from the presets folder\n"), m_loaded_slots, m_loaded_dropins);
        }
    }

    // The Dawnwalker Mod Menu (Nexus 271) creates its host as a plain CommonActivatableWidget (main.lua, library:Create
    // with the native class, which the game's own screens all subclass), enabled and shown while the menu is open and
    // Collapsed once closed, when it lingers until GC. Any such widget not Collapsed means a settings page may be open.
    // Only reached with a write pending and the camera not live. A menu that changes its host is not recognised, and
    // the write then lands under the open page as it did before 0.9: the page refuses its next Apply until reopened.
    auto mod_menu_open() -> bool
    {
        constexpr uint8_t COLLAPSED = 1; // ESlateVisibility
        auto shown = [&](UObject* object) -> bool {
            auto* visibility = object->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
            return visibility && *visibility != COLLAPSED;
        };
        if (!m_activatable_class)
        {
            m_activatable_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/CommonUI.CommonActivatableWidget"));
            if (!m_activatable_class) return false;
        }
        if (m_menu_host && !m_menu_host->IsUnreachable() && m_menu_host->GetClassPrivate() == m_activatable_class && shown(m_menu_host)) return true;
        m_menu_host = nullptr; // closed or gone; every open creates a new host
        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
            if (!object || object->GetClassPrivate() != m_activatable_class || object->IsUnreachable() || !shown(object)) return LoopAction::Continue;
            m_menu_host = object;
            return LoopAction::Break;
        });
        if (m_menu_host && !m_menu_logged)
        {
            m_menu_logged = true;
            if (dw::verbose())
                Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] Mod Menu open: {}\n"), m_menu_host->GetFullName());
        }
        return m_menu_host != nullptr;
    }

    auto save_slot_locked(int slot, settings::Values values) -> bool
    {
        values = settings::normalize_preset(values);
        CreateDirectoryA(PRESETS_DIR, nullptr);
        auto path = std::string(PRESETS_DIR) + "/Slot " + std::to_string(slot) + ".ini";
        auto* known = find_preset(slot);
        auto name = known ? known->name : "Slot " + std::to_string(slot); // a renamed slot keeps its name
        if (!settings::write_file(path, settings::slot_file_content(slot, name, values))) return false;
        if (known)
        {
            known->values = std::move(values);
            return true;
        }
        auto is_after = [&](const settings::Preset& p) { return (p.id >= 1 && p.id <= settings::MAX_SLOTS && p.id > slot) || p.id >= settings::FIRST_DROPIN_ID; };
        m_presets.insert(std::find_if(m_presets.begin(), m_presets.end(), is_after), settings::Preset{slot, "Slot " + std::to_string(slot), std::move(values)});
        return true;
    }

    auto find_preset(int id) -> settings::Preset*
    {
        auto it = std::find_if(m_presets.begin(), m_presets.end(), [&](const settings::Preset& p) { return p.id == id; });
        return it != m_presets.end() ? &*it : nullptr;
    }

    // Applies only the keys the preset holds; a hand-edited file may hold fewer.
    auto load_preset_locked(int id) -> bool
    {
        auto* preset = find_preset(id);
        if (!preset)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] preset {} is empty, nothing loaded\n"), id);
            return false;
        }
        settings::apply_values(m_settings, preset->values);
        m_loaded_id = id;
        m_custom_pinned = false;
        auto name = dw::to_wide(preset->name);
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] loaded preset {}\n"), name);
        request_banner(STR("Smoothwalker: ") + name);
        return true;
    }

    // m_settings.preset becomes the preset the live settings match, preferring the last one loaded, else 0 (Custom).
    auto update_active_locked() -> void
    {
        auto matches = [&](const settings::Preset& p) {
            return !p.values.empty() && std::all_of(p.values.begin(), p.values.end(), [&](auto& entry) {
                return std::abs(settings::number_of(m_settings, entry.first) - entry.second) <= 1e-4;
            });
        };
        if (m_custom_pinned)
        {
            m_settings.preset = 0;
            return;
        }
        int active = 0;
        if (auto* loaded = m_loaded_id != 0 ? find_preset(m_loaded_id) : nullptr; loaded && matches(*loaded)) active = m_loaded_id;
        for (auto& p : m_presets)
        {
            if (!active && matches(p)) active = p.id;
        }
        m_settings.preset = active;
    }

    // Built-ins, saved slots, then drop-ins, starting after the active preset (at the first from Custom).
    auto cycle_preset() -> void
    {
        std::lock_guard guard(m_file_mutex);
        auto at = std::find_if(m_presets.begin(), m_presets.end(), [&](const settings::Preset& p) { return p.id == m_settings.preset; });
        size_t next = m_settings.preset == 0 || at == m_presets.end() ? 0 : (static_cast<size_t>(at - m_presets.begin()) + 1) % m_presets.size();
        if (!load_preset_locked(m_presets[next].id)) return;
        update_active_locked();
        publish_locked();
        mark_pending_locked();
    }

    // The live numbers that differ from the file; preset is written as m_settings holds it (the active preset).
    // Compared as written (%.6g), so a value the file cannot hold exactly is not rewritten forever.
    auto pending_writes_locked() -> settings::Values
    {
        settings::Values writes;
        for (auto* key : settings::NUMERIC_KEYS)
        {
            double value = settings::number_of(m_settings, key);
            auto known = m_baseline.find(key);
            if (known == m_baseline.end() || settings::format_number(known->second) != settings::format_number(value)) writes.emplace_back(key, value);
        }
        return writes;
    }

    // After any change to the live settings, the baseline or the stamp. Marks the write-back and mirrors it into
    // smoothwalker.pending (which the Mod Menu does not read), so a preset loaded from a paused menu survives an exit
    // or hot reload before the camera goes live. The side file is rewritten only when its content changes.
    auto mark_pending_locked() -> void
    {
        auto writes = pending_writes_locked();
        m_flush_pending.store(!writes.empty());
        std::string content;
        if (!writes.empty())
        {
            content = "; DWSmoothwalker: settings not yet written to smoothwalker.ini. Applied at the next start only while\n"
                      "; smoothwalker.ini's write time still equals stamp.\n"
                      "stamp = " + std::to_string(m_settings_stamp) + "\n";
            char buffer[64];
            for (auto& [key, value] : writes)
            {
                std::snprintf(buffer, sizeof(buffer), "%.17g", value);
                content += key + " = " + buffer + "\n";
            }
        }
        if (content == m_pending_file) return;
        if (content.empty())
        {
            DeleteFileA(PENDING_PATH);
        }
        else if (!settings::write_file(PENDING_PATH, content))
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not write smoothwalker.pending\n"));
            return;
        }
        m_pending_file = std::move(content);
    }

    // Once, in the constructor after the full parse, so the Mod Menu never reads a file without them: an ini copied
    // back from an older version lacks the newer keys, and the page opens only if every ConfigKey is in the file.
    // The added lines hold the live values, so the baseline takes them as written, as the flush does.
    auto add_missing_keys_locked() -> void
    {
        if (last_write(SETTINGS_PATH) != m_settings_stamp) return; // missing (stamp 0 both), or changed since the parse
        auto content = settings::read_file(SETTINGS_PATH);
        if (!content) return;
        auto [updated, added] = settings::with_missing_keys(*content, m_settings);
        if (added.empty()) return;
        if (!settings::write_file(SETTINGS_PATH, updated))
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] smoothwalker.ini: could not add {} missing keys\n"), added.size());
            return;
        }
        m_settings_stamp = last_write(SETTINGS_PATH);
        std::string names;
        for (auto& key : added)
        {
            if (settings::is_numeric_key(key)) m_baseline[key] = std::stod(settings::format_number(settings::number_of(m_settings, key))); // debug_key is a name
            names += (names.empty() ? "" : ", ") + key;
        }
        mark_pending_locked(); // the added keys are no longer pending, and the side file takes the new stamp
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] smoothwalker.ini: added {} missing keys: {}\n"), added.size(), widen(names));
    }

    // A write-back the last session missed. Applied only if smoothwalker.ini still has the stamped write time.
    auto apply_pending_file_locked() -> void
    {
        auto content = settings::read_file(PENDING_PATH);
        if (!content) return;
        DeleteFileA(PENDING_PATH);

        std::optional<uint64_t> stamp;
        std::istringstream in(*content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos || settings::trim(line.substr(0, eq)) != "stamp") continue;
            try
            {
                stamp = std::stoull(settings::trim(line.substr(eq + 1)));
            }
            catch (...)
            {
            }
        }
        if (!stamp || *stamp != m_settings_stamp)
        {
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] smoothwalker.pending ignored: smoothwalker.ini changed since\n"));
            return;
        }
        auto numbers = settings::parse_numbers(*content);
        settings::apply_values(m_settings, settings::Values(numbers.begin(), numbers.end()));
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] applied {} settings the last session had not written yet\n"), numbers.size());
    }

    // Runs only while the camera is live, so no Mod Menu page is open to refuse its next Apply over the change.
    auto flush_locked(std::chrono::steady_clock::time_point now) -> void
    {
        // The file changed since last read (or is gone): let the poll handle it first, throttled so a deleted
        // file is not queried every tick.
        if (last_write(SETTINGS_PATH) != m_settings_stamp)
        {
            m_next_flush = now + std::chrono::milliseconds(250);
            return;
        }

        auto writes = pending_writes_locked();
        if (writes.empty())
        {
            mark_pending_locked();
            return;
        }

        auto content = settings::read_file(SETTINGS_PATH);
        auto updated = content ? settings::rewrite_numbers(*content, writes) : std::string{};
        bool changed = content && updated != *content;
        if (!content || (changed && !settings::write_file(SETTINGS_PATH, updated)))
        {
            if (!m_flush_failing) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not write smoothwalker.ini, retrying\n"));
            m_flush_failing = true;
            m_next_flush = now + std::chrono::milliseconds(250);
            return;
        }
        if (changed) m_settings_stamp = last_write(SETTINGS_PATH);
        // A key missing from the file counts as written too, so it is not retried every tick.
        for (auto& [key, value] : writes) m_baseline[key] = std::stod(settings::format_number(value));
        m_flush_failing = false;
        mark_pending_locked(); // nothing left: clears the flag and deletes smoothwalker.pending
    }

    // The debug overlay's panel, gathered at its refresh (a quarter second apart) on the game thread: the core's feed
    // (CameraCore::read_debug), the follow's settings under their shared lock, the copy publish_locked leaves in
    // m_debug_*, the tuner and the API claim (CameraCore::camera_owner). Never m_file_mutex, which the engine tick
    // does not take.
    auto debug_panel(UObject* camera) -> ui::DebugPanel
    {
        ui::DebugPanel p;
        p.enabled = m_processor.enabled();
        {
            std::lock_guard guard(m_debug_mutex);
            p.preset = m_debug_preset;
            p.camera_tuning = m_debug_tuning;
        }
        p.camera_type = m_tuner.camera_type_name(camera);
        p.modes = m_tuner.list_modes(camera, p.modes_stale);
        const follow::FollowTuning follow = m_processor.tuning();
        p.max_lag_h = follow.max_lag_h;
        p.max_lag_v = follow.max_lag_v;
        p.rotation_smoothing = follow.rotation_smoothing;
        const camera::DebugFeed feed = m_core.read_debug();
        p.keep_follow = feed.keep_follow;
        p.keep_turn = feed.keep_turn;
        p.influence = static_cast<follow::Influence>(feed.influence);
        p.lag_h = feed.lag_h;
        p.lag_v = feed.lag_v;
        p.rate_h = feed.rate_h;
        p.snap = static_cast<camera::Snap>(feed.snap);
        p.snap_age = feed.snap_age;
        p.api = api_status();
        p.api.glide = feed.glide;
        return p;
    }
};

Smoothwalker::Smoothwalker(camera::CameraCore& core, BindKey bind_key, std::wstring version)
    : m(std::make_unique<Impl>(core, std::move(bind_key), std::move(version)))
{
}

Smoothwalker::~Smoothwalker() = default;

auto Smoothwalker::start() -> void
{
    m->start();
}

auto Smoothwalker::update() -> void
{
    m->update();
}

auto Smoothwalker::unregister_callbacks() -> void
{
    for (auto id : m->m_callbacks) Hook::UnregisterCallback(id);
    m->m_callbacks.clear();
}

// After the core unhooked and waited for the hook (mod.cpp): the game thread is out of m_tuner (the callbacks
// are gone), and no hook call is inside the processor. The CDOs get their base back; then the follow and the
// listener leave the core. Everything of this side lives in this object and goes with it; the verbose flag it
// writes is rewritten by the next instance's constructor. False: a core call into the follow or the listener is still
// running, so the Impl (the follow, the tuner, the listener's target) must stay allocated; the mod then leaks this
// object and the core together and logs it (mod.cpp). The core cleared both slots before it timed out, so no new
// call reaches a leaked Impl.
auto Smoothwalker::shutdown() -> bool
{
    m->m_tuner.restore();
    // Both waits always run: the second must not be skipped by the first timing out.
    const bool listener_drained = m->m_core.clear_listener(m->m_hooks);
    const bool processor_drained = m->m_core.unregister_processor(m->m_processor);
    return listener_drained && processor_drained;
}
} // namespace dw::smoothwalker
