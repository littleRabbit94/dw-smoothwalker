// The settings file state machine (docs/design.md, "The settings file" and "Presets"): the live settings, the baseline a
// Mod Menu Apply is diffed against, reload rules (a)-(e), the active preset and the Custom pin, slots and the presets
// scan, the deferred write-back with its retry, the pending side file and the missing-keys writer. Every file access
// goes through Files (Win32Files in the mod, an in-memory one in tests); log lines, banners, publishes and the switch
// go out through Events, in the order they happen. No UE4SS or Unreal type: the Mod Menu check (mod_menu_open) and
// the camera_live() gate stay with the orchestrator (smoothwalker.cpp), which decides when flush_locked runs.
#pragma once

#include "presets.hpp"
#include "settings.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dw::smoothwalker::settings
{
    // Not under scripts/: UE4SS makes a Lua mod of any folder with a scripts subfolder and logs a missing main.lua.
    inline constexpr const char* SETTINGS_PATH = "ue4ss/Mods/DWSmoothwalker/config/smoothwalker.ini";
    inline constexpr const char* PENDING_PATH = "ue4ss/Mods/DWSmoothwalker/config/smoothwalker.pending";
    inline constexpr const char* PRESETS_DIR = "ue4ss/Mods/DWSmoothwalker/config/presets";
    inline constexpr const wchar_t* PRESETS_DIR_W = L"ue4ss\\Mods\\DWSmoothwalker\\config\\presets";
    inline constexpr const char* MANIFEST_PATH = "ue4ss/Mods/DWSmoothwalker/mod_settings.ini";

    // Every file access the store makes. Paths are relative to the game's working directory.
    struct Files
    {
        virtual ~Files() = default;
        virtual auto read(const std::string& path) -> std::optional<std::string> = 0;                     // nullopt: cannot open
        virtual auto read_small(const std::wstring& path, size_t limit) -> std::optional<std::string> = 0; // nullopt: over limit or unreadable
        virtual auto write(const std::string& path, const std::string& content) -> bool = 0;             // temp file plus rename
        virtual auto mtime(const std::string& path) -> uint64_t = 0;                                      // last write time; 0: missing
        virtual auto remove(const std::string& path) -> void = 0;
        virtual auto mkdir(const std::string& path) -> void = 0;  // fails harmlessly if present
        virtual auto mkdir(const std::wstring& path) -> void = 0;
        virtual auto list(const std::wstring& dir) -> PresetFiles = 0; // the presets folder's slot and drop-in files
        virtual auto wait(int milliseconds) -> void = 0;              // the startup read rides out a Mod Menu rename
    };

    // The mod's Files: ini.hpp's read_file / write_file, presets.hpp's read_small_file / list_preset_files, and Win32.
    class Win32Files final : public Files
    {
      public:
        auto read(const std::string& path) -> std::optional<std::string> override;
        auto read_small(const std::wstring& path, size_t limit) -> std::optional<std::string> override;
        auto write(const std::string& path, const std::string& content) -> bool override;
        auto mtime(const std::string& path) -> uint64_t override;
        auto remove(const std::string& path) -> void override;
        auto mkdir(const std::string& path) -> void override;
        auto mkdir(const std::wstring& path) -> void override;
        auto list(const std::wstring& dir) -> PresetFiles override;
        auto wait(int milliseconds) -> void override;
    };

    enum class Level
    {
        Normal,
        Verbose, // sent only while dw::verbose(): the store checks before it builds the line
        Warning,
    };

    // What the store hands the orchestrator, synchronously and in order, from inside a call made under mutex().
    struct Events
    {
        virtual ~Events() = default;
        virtual auto log(Level level, const std::wstring& line) -> void = 0; // a whole line, "[DWSmoothwalker] ...\n"
        virtual auto banner(const std::wstring& text) -> void = 0;         // request_banner
        virtual auto publish() -> void = 0;                                 // the live settings changed: publish_locked
        virtual auto set_enabled(bool on) -> void = 0;                      // the file's enabled moved: switch with a fade
        virtual auto store_enabled(bool on) -> void = 0;                    // startup: the switch as the file has it, no fade
    };

    class SettingsStore
    {
      public:
        SettingsStore(Files& files, Events& events) : m_files(files), m_events(events) {}
        SettingsStore(const SettingsStore&) = delete;
        auto operator=(const SettingsStore&) -> SettingsStore& = delete;

        // The config files, the live settings, the baseline, the presets and the loaded id. Every _locked method runs
        // under it; the engine tick never takes it.
        auto mutex() -> std::mutex& { return m_file_mutex; }

        // --------------------------------------------------------------------------------------- under mutex()

        // The component's construction: log_verbose read early, the presets scan, the full parse, the missing keys,
        // then one flush without the camera_live() gate (docs/design.md, "Startup flush") at steady_clock::now().
        auto start_locked() -> void;
        // The 250 ms poll: a changed write time reloads (rules (a)-(e)).
        auto poll_locked() -> void;
        // Writes the live numbers that differ from the file. The caller gates it: the camera is live, or the Mod Menu
        // is closed (flush_due first).
        auto flush_locked(std::chrono::steady_clock::time_point now) -> void;
        // The preset key: built-ins, saved slots, then drop-ins, after the active preset (the first from Custom).
        auto cycle_locked() -> void;
        auto update_active_locked() -> void;
        auto mark_pending_locked() -> void;
        auto find_preset(int id) -> Preset*;
        auto settings() -> Settings& { return m_settings; }

        // Read once, by the startup parse.
        auto toggle_key() const -> const std::string& { return m_toggle_key; }
        auto preset_key() const -> const std::string& { return m_preset_key; }
        auto shoulder_key() const -> const std::string& { return m_shoulder_key; }
        auto debug_key() const -> const std::string& { return m_debug_key; }
        // The scan's counts, for the load line.
        auto loaded_slots() const -> size_t { return m_loaded_slots; }
        auto loaded_dropins() const -> size_t { return m_loaded_dropins; }

        // ------------------------------------------------------------------------------------------ no lock

        // A write-back is pending and its retry time has come.
        auto flush_due(std::chrono::steady_clock::time_point now) const -> bool { return m_flush_pending.load() && now >= m_next_flush; }

        // Read-only, for a single-threaded check (tests).
        auto baseline() const -> const std::map<std::string, double>& { return m_baseline; }
        auto presets() const -> const std::vector<Preset>& { return m_presets; }
        auto loaded_id() const -> int { return m_loaded_id; }
        auto custom_pinned() const -> bool { return m_custom_pinned; }
        auto stamp() const -> uint64_t { return m_settings_stamp; }
        auto pending_file() const -> const std::string& { return m_pending_file; }
        auto flush_failing() const -> bool { return m_flush_failing; }
        auto next_flush() const -> std::chrono::steady_clock::time_point { return m_next_flush; }

      private:
        auto log(Level level, const std::wstring& line) -> void;
        auto reload_settings_locked(bool startup) -> void;
        auto load_presets_locked() -> void;
        auto save_slot_locked(int slot, Values values) -> bool;
        auto load_preset_locked(int id) -> bool;
        auto pending_writes_locked() -> Values;
        auto add_missing_keys_locked() -> void;
        auto apply_pending_file_locked() -> void;

        Files& m_files;
        Events& m_events;

        std::mutex m_file_mutex;
        Settings m_settings{};
        uint64_t m_settings_stamp = 0;                // write time of smoothwalker.ini as last read or written
        std::map<std::string, double> m_baseline;     // each numeric key as last known to be in smoothwalker.ini
        int m_loaded_id = 0;                          // last preset loaded or cycled; shown while it still matches
        bool m_custom_pinned = false;                 // Custom was picked: shown until a preset is loaded or a slot adopted
        std::vector<Preset> m_presets;                // built-ins, slots, drop-ins, in cycle order; scanned once per session
        size_t m_loaded_slots = 0, m_loaded_dropins = 0; // load_presets_locked()'s counts, for the constructor's load line
        std::string m_pending_file;                   // content last written to smoothwalker.pending; empty when none
        std::atomic<bool> m_flush_pending{false};     // live settings differ from m_baseline
        bool m_flush_failing = false;                 // a write-back failed; warned once until one succeeds
        std::chrono::steady_clock::time_point m_next_flush{};
        std::string m_toggle_key, m_preset_key, m_shoulder_key, m_debug_key;
    };
} // namespace dw::smoothwalker::settings
