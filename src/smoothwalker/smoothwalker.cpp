// Smoothwalker's side of the DLL, as orchestration (docs/design.md, "Core and processors"): it owns the parts and wires
// them to each other and to the camera core's interface (camera/api.hpp): the follow processor (follow/processor.hpp),
// the settings store (settings/store.hpp), the camera modes' tuner (modes/mode_tuner.hpp), the banners, the Mod Menu
// probe and the debug overlay (ui/). What stays here is construction, start, update and shutdown, the key binds, the
// core's game-thread listener, publish (the live settings out to the follow, the modes and the panel), apply_position,
// the debug panel's gathering and the store's event forwarder.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "smoothwalker.hpp"
#include "follow/processor.hpp"
#include "modes/mode_tuner.hpp"
#include "settings/store.hpp"
#include "ui/banner.hpp"
#include "ui/debug_overlay.hpp"
#include "ui/menu_probe.hpp"
#include "ui/panel.hpp"
#include "../common/live_ref.hpp"
#include "../common/log.hpp"
#include "../common/text.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace dw::smoothwalker
{
namespace
{
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

} // namespace

struct Smoothwalker::Impl
{
    camera::CameraCore& m_core;            // the camera core's interface: everything this side knows of the core
    BindKey m_bind_key;
    std::wstring m_version;
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    follow::FollowProcessor m_processor;   // registered with the core for this object's life

    // smoothwalker.ini, the presets and the live settings (settings/store.hpp), under its own mutex, which the engine
    // tick never takes. Its log lines, banners, publishes and switch changes come back through StoreEvents, in order,
    // while the caller holds that mutex.
    struct StoreEvents final : settings::Events
    {
        Impl& self;
        explicit StoreEvents(Impl& impl) : self(impl) {}
        auto log(settings::Level level, const std::wstring& line) -> void override
        {
            if (level == settings::Level::Warning) Output::send<LogLevel::Warning>(STR("{}"), line);
            else if (level == settings::Level::Verbose) Output::send<LogLevel::Verbose>(STR("{}"), line);
            else Output::send<LogLevel::Normal>(STR("{}"), line);
        }
        auto banner(const std::wstring& text) -> void override { self.m_banner.request(text); }
        auto publish() -> void override { self.publish_locked(); }
        auto set_enabled(bool on) -> void override { self.set_enabled_locked(on); }
        auto store_enabled(bool on) -> void override { self.m_processor.store_enabled(on); }
    };
    settings::Win32Files m_files;
    StoreEvents m_store_events{*this};
    settings::SettingsStore m_store{m_files, m_store_events};

    ui::Banner m_banner;                   // any thread requests, the game thread shows
    ui::MenuProbe m_menu;                  // game thread only
    ui::DebugOverlay m_overlay;            // game thread only
    std::atomic<bool> m_debug_overlay{false};
    std::mutex m_debug_mutex;              // m_debug_preset, m_debug_tuning: written by publish_locked, read by the overlay's refresh
    std::wstring m_debug_preset;           // the active preset's name, or Custom
    bool m_debug_tuning = true;            // camera_tuning

    modes::ModeTuner m_tuner;              // game thread only
    std::mutex m_position_mutex;           // m_position
    modes::PositionTuning m_position{};
    std::atomic<uint64_t> m_position_generation{1};
    uint64_t m_position_applied_generation = 0; // game thread only

    std::chrono::steady_clock::time_point m_last_report{}, m_last_poll{};

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

    Impl(camera::CameraCore& core, BindKey bind_key, std::wstring version)
        : m_core(core), m_bind_key(std::move(bind_key)), m_version(std::move(version)), m_banner(core)
    {
        if (!m_core.register_processor(m_processor))
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] the camera core already runs another view processor: smoothing inactive\n"));
        if (!m_core.set_listener(m_hooks))
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] the camera core already has a listener: camera position tuning inactive\n"));

        std::lock_guard guard(m_store.mutex());
        m_store.start_locked(); // log_verbose, the presets scan, the parse, the missing keys, the startup flush
        const size_t slots = m_store.loaded_slots(), dropins = m_store.loaded_dropins();
        auto s = [](size_t n) { return n == 1 ? STR("") : STR("s"); };
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] v{} loaded, {}, {} saved slot{}, {} preset{} from the presets folder\n"), m_version,
                                       m_processor.enabled() ? STR("on") : STR("off"), slots, s(slots), dropins, s(dropins));
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

        bind(m_store.toggle_key(), STR("toggle_key"), [this]() {
            std::lock_guard guard(m_store.mutex());
            bool now = !m_processor.enabled();
            set_enabled_locked(now);
            publish_locked();
            m_store.mark_pending_locked();
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] smoothing {}\n"), now ? STR("on") : STR("off"));
            m_banner.request(now ? STR("Smoothwalker: On") : STR("Smoothwalker: Off"));
        });
        bind(m_store.preset_key(), STR("preset_key"), [this]() {
            if (key_live(STR("preset"))) cycle_preset();
        });
        bind(m_store.shoulder_key(), STR("shoulder_key"), [this]() {
            if (key_live(STR("shoulder"))) swap_shoulder();
        });
        // Written back like the toggle key's change, so the Mod Menu page shows it. Live under a pause too: it moves
        // nothing in the game.
        bind(m_store.debug_key(), STR("debug_key"), [this]() {
            std::lock_guard guard(m_store.mutex());
            m_store.settings().debug_overlay = !m_store.settings().debug_overlay;
            publish_locked();
            m_store.mark_pending_locked();
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] debug overlay {}\n"), m_store.settings().debug_overlay ? STR("on") : STR("off"));
        });

        m_last_report = m_last_poll = std::chrono::steady_clock::now();
    }

    auto update() -> void
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - m_last_poll).count() >= 0.25)
        {
            m_last_poll = now;
            std::lock_guard guard(m_store.mutex());
            m_store.poll_locked();
        }
        // After the poll, so a file change is applied before the live values are written over it. Written while the
        // camera is live, or paused with the Mod Menu closed: a page reopened from the pause menu then shows the
        // loaded values, and no page is open to refuse its next Apply over the write.
        if (m_store.flush_due(now) && (m_core.camera_live() || !m_menu.open()))
        {
            std::lock_guard guard(m_store.mutex());
            m_store.flush_locked(now);
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
        m_banner.show_pending();
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

    // The live switch, kept in the live settings too so the write-back shows it on the Mod Menu page. A change fades
    // (FollowProcessor::set_enabled).
    auto set_enabled_locked(bool on) -> void
    {
        m_store.settings().enabled = on;
        m_processor.set_enabled(on);
    }

    auto publish_locked() -> void
    {
        // No hard cut: the core crossfades on the new generations instead of snapping the lag away mid-motion. Only a
        // changed value starts one: a shoulder swap and the switches change none, and a fade holds part of the old lag.
        follow::publish_view(m_processor, m_core, m_store.settings());
        m_banner.enable(m_store.settings().show_banner);
        m_debug_overlay.store(m_store.settings().debug_overlay);
        {
            auto* active = m_store.settings().preset != 0 ? m_store.find_preset(m_store.settings().preset) : nullptr;
            std::lock_guard guard(m_debug_mutex);
            m_debug_preset = m_store.settings().preset == 0 ? STR("Custom") : active ? dw::to_wide(active->name) : STR("preset ") + std::to_wstring(m_store.settings().preset);
            m_debug_tuning = m_store.settings().camera_tuning;
        }

        // enabled off is the game as shipped, camera modes and its own lag included (position_of).
        auto position = modes::position_of(m_store.settings());
        std::lock_guard guard(m_position_mutex);
        // Only what modes::written() keeps counts as a change; the values are kept, so switching camera_tuning on
        // applies the latest.
        if (!(modes::written(position) == modes::written(m_position))) m_position_generation.fetch_add(1);
        m_position = position;
    }

    // Written back to the ini by flush_locked, so the Mod Menu page shows it.
    auto swap_shoulder() -> void
    {
        std::lock_guard guard(m_store.mutex());
        m_store.settings().shoulder_swap = !m_store.settings().shoulder_swap;
        m_store.update_active_locked();
        publish_locked();
        m_store.mark_pending_locked();
        // No banner: the camera moving to the other shoulder is the feedback.
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] shoulder {}{}\n"), m_store.settings().shoulder_swap ? STR("swapped") : STR("as the game has it"),
                                       m_store.settings().camera_tuning ? STR("") : STR(" (camera_tuning is 0: shows once it is on)"));
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

    // Built-ins, saved slots, then drop-ins, starting after the active preset (at the first from Custom).
    auto cycle_preset() -> void
    {
        std::lock_guard guard(m_store.mutex());
        m_store.cycle_locked();
    }

    // The debug overlay's panel, gathered at its refresh (a quarter second apart) on the game thread: the core's feed
    // (CameraCore::read_debug), the follow's settings under their shared lock, the copy publish_locked leaves in
    // m_debug_*, the tuner and the API claim (CameraCore::camera_owner). Never the store's mutex, which the engine
    // tick does not take.
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
