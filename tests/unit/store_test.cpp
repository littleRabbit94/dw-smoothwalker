// smoothwalker/settings/store: the settings file state machine against docs/design.md "The settings file" and
// "Presets": startup (missing file, the rename retry, the early log_verbose, the missing keys only while the stamp is
// unchanged, the startup flush), the diff-based reload rules (a)-(e), slots as profiles and the Custom pin, the cycle,
// the deferred write-back and its retry, and the pending side file. Files in memory; the shipped smoothwalker.ini and
// mod_settings.ini as fixtures.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "check.hpp"
#include "common/log.hpp"
#include "smoothwalker/settings/ini.hpp"
#include "smoothwalker/settings/presets.hpp"
#include "smoothwalker/settings/settings.hpp"
#include "smoothwalker/settings/store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dw::smoothwalker::settings;
using std::chrono::milliseconds;

namespace
{
    // Past any steady_clock::now() the store reads itself (start_locked's flush), so a retry it schedules is due by T0.
    const auto T0 = std::chrono::steady_clock::now() + std::chrono::hours(1);

    auto fixture(const char* relative) -> std::string
    {
        std::ifstream file(std::string(DW_REPO) + "/" + relative, std::ios::binary);
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }

    auto lower(std::string s) -> std::string
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Files in memory. Paths are keyed with forward slashes, so the store's narrow and wide spellings of one file meet.
    // Every write moves the clock, so the write time says which write was last.
    class MemoryFiles final : public Files
    {
      public:
        struct Entry
        {
            std::string content;
            uint64_t mtime;
        };
        std::map<std::string, Entry> files;
        std::set<std::string> dirs;
        uint64_t clock = 1000;
        std::set<std::string> failing;                         // writes to these paths fail
        std::map<std::string, int> writes;                     // successful writes per path
        int waits = 0;
        std::function<void(const std::string&)> after_read;    // runs after a read has taken its content
        std::function<void()> on_wait;

        static auto key(std::string p) -> std::string
        {
            std::replace(p.begin(), p.end(), '\\', '/');
            return p;
        }
        static auto key(const std::wstring& w) -> std::string
        {
            std::string p;
            for (wchar_t c : w) p += static_cast<char>(c);
            return key(p);
        }

        // The Mod Menu (or a hand edit) replaces the file.
        auto put(const std::string& path, const std::string& content) -> void { files[key(path)] = {content, ++clock}; }
        auto content(const std::string& path) const -> std::optional<std::string>
        {
            auto it = files.find(key(path));
            return it == files.end() ? std::nullopt : std::optional<std::string>(it->second.content);
        }
        // An Apply: the menu rewrites the numbers it changed, in place.
        auto menu_apply(const Values& edits) -> void { put(SETTINGS_PATH, rewrite_numbers(content(SETTINGS_PATH).value_or(""), edits)); }
        auto number(const std::string& path, const std::string& k) const -> std::optional<double>
        {
            auto n = parse_numbers(content(path).value_or(""));
            auto it = n.find(k);
            return it == n.end() ? std::nullopt : std::optional<double>(it->second);
        }

        auto read(const std::string& path) -> std::optional<std::string> override
        {
            auto out = content(path);
            if (after_read) after_read(key(path));
            return out;
        }
        auto read_small(const std::wstring& path, size_t limit) -> std::optional<std::string> override
        {
            auto out = content(key(path));
            if (!out || out->size() > limit) return std::nullopt;
            return out;
        }
        auto write(const std::string& path, const std::string& text) -> bool override
        {
            if (failing.count(key(path))) return false;
            put(path, text);
            ++writes[key(path)];
            return true;
        }
        auto mtime(const std::string& path) -> uint64_t override
        {
            auto it = files.find(key(path));
            return it == files.end() ? 0 : it->second.mtime;
        }
        auto remove(const std::string& path) -> void override { files.erase(key(path)); }
        auto mkdir(const std::string& path) -> void override { dirs.insert(key(path)); }
        auto mkdir(const std::wstring& path) -> void override { dirs.insert(key(path)); }
        auto list(const std::wstring& dir) -> PresetFiles override
        {
            PresetFiles out;
            const std::string prefix = key(dir) + "/";
            for (auto& [path, entry] : files)
            {
                if (!path.starts_with(prefix)) continue;
                std::string name = path.substr(prefix.size());
                if (name.find('/') != std::string::npos || !lower(name).ends_with(".ini") || lower(name) == "mod_settings.ini") continue;
                std::wstring wide(name.begin(), name.end());
                int slot = 0;
                for (int n = 1; n <= MAX_SLOTS; ++n)
                    if (lower(name) == "slot " + std::to_string(n) + ".ini") slot = n;
                if (slot) out.slots[slot] = wide;
                else out.dropins.push_back(wide);
            }
            std::sort(out.dropins.begin(), out.dropins.end(), [](const std::wstring& a, const std::wstring& b) {
                std::string x, y;
                for (wchar_t c : a) x += static_cast<char>(c);
                for (wchar_t c : b) y += static_cast<char>(c);
                return lower(x) < lower(y);
            });
            return out;
        }
        auto wait(int) -> void override
        {
            ++waits;
            if (on_wait) on_wait();
        }
    };

    // The orchestrator's side, recorded in order. set_enabled writes the switch into the live settings as
    // set_enabled_locked does.
    class Recorder final : public Events
    {
      public:
        SettingsStore* store = nullptr;
        std::vector<std::string> order; // "log", "banner", "publish", "enabled", "stored"
        std::vector<std::pair<Level, std::wstring>> lines;
        std::vector<std::wstring> banners;
        std::vector<bool> enabled, stored;
        int publishes = 0;

        auto log(Level level, const std::wstring& line) -> void override
        {
            order.push_back("log");
            lines.emplace_back(level, line);
        }
        auto banner(const std::wstring& text) -> void override
        {
            order.push_back("banner");
            banners.push_back(text);
        }
        auto publish() -> void override
        {
            order.push_back("publish");
            ++publishes;
        }
        auto set_enabled(bool on) -> void override
        {
            order.push_back("enabled");
            enabled.push_back(on);
            store->settings().enabled = on;
        }
        auto store_enabled(bool on) -> void override
        {
            order.push_back("stored");
            stored.push_back(on);
        }

        auto logged(const std::wstring& line, Level level = Level::Normal) const -> bool
        {
            return std::find(lines.begin(), lines.end(), std::make_pair(level, line)) != lines.end();
        }
        auto clear() -> void
        {
            order.clear();
            lines.clear();
            banners.clear();
            enabled.clear();
            stored.clear();
            publishes = 0;
        }
    };

    // One component instance: a store over the shared files.
    struct Session
    {
        Recorder events;
        SettingsStore store;
        explicit Session(MemoryFiles& files) : store(files, events) { events.store = &store; }
        auto start() -> void
        {
            std::lock_guard guard(store.mutex());
            store.start_locked();
        }
        auto poll() -> void
        {
            std::lock_guard guard(store.mutex());
            store.poll_locked();
        }
        auto flush(std::chrono::steady_clock::time_point now = T0) -> void
        {
            std::lock_guard guard(store.mutex());
            store.flush_locked(now);
        }
        auto cycle() -> void
        {
            std::lock_guard guard(store.mutex());
            store.cycle_locked();
        }
        // A key press that changes a live value (N, O, the debug key): changed, then marked pending.
        auto live(const std::function<void(Settings&)>& change) -> void
        {
            std::lock_guard guard(store.mutex());
            change(store.settings());
            store.update_active_locked();
            store.mark_pending_locked();
        }
        auto s() -> Settings& { return store.settings(); }
    };

    // The game folder as shipped: smoothwalker.ini and mod_settings.ini, no presets folder yet.
    auto shipped(const Values& edits = {}) -> MemoryFiles
    {
        MemoryFiles files;
        files.put(SETTINGS_PATH, rewrite_numbers(fixture("mod/config/smoothwalker.ini"), edits));
        files.put(MANIFEST_PATH, fixture("mod/mod_settings.ini"));
        return files;
    }

    auto slot_path(int n) -> std::string { return std::string(PRESETS_DIR) + "/Slot " + std::to_string(n) + ".ini"; }
    auto dropin_path(const std::string& name) -> std::string { return std::string(PRESETS_DIR) + "/" + name; }

    auto manifest_line(const MemoryFiles& files, const std::string& k) -> std::string
    {
        std::istringstream in(files.content(MANIFEST_PATH).value_or(""));
        std::string line;
        bool section = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("[")) section = line == "[Setting.preset]";
            else if (section && line.starts_with(k + " = ")) return line.substr(k.size() + 3);
        }
        return {};
    }
} // namespace

// ------------------------------------------------------------------------------------------------------ startup

TEST(store, startup_as_shipped)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    CHECK_EQ(a.s().preset, 102); // Balanced matches the shipped values
    CHECK_EQ(a.store.loaded_id(), 102);
    CHECK_EQ(a.store.shoulder_key(), std::string("V"));
    CHECK(a.store.toggle_key().empty());
    CHECK(!a.store.flush_due(T0));
    CHECK_EQ(files.writes[SETTINGS_PATH], 0);  // nothing to add, nothing to flush
    CHECK_EQ(files.writes[MANIFEST_PATH], 0);  // the picker lines already match: not rewritten
    CHECK(!files.content(PENDING_PATH));
    CHECK(files.dirs.count(MemoryFiles::key(PRESETS_DIR_W)) == 1); // created so there is a folder to drop files into
    CHECK_EQ(a.events.stored.size(), 1u);
    CHECK(a.events.stored.size() == 1 && a.events.stored[0]);
    CHECK(a.events.publishes >= 1);
    CHECK_EQ(a.store.presets().size(), 3u);
    CHECK_EQ(a.store.loaded_slots(), 0u);
    CHECK_EQ(a.store.loaded_dropins(), 0u);
    CHECK_EQ(a.store.stamp(), files.mtime(SETTINGS_PATH));
}

TEST(store, startup_file_values_and_the_startup_flush)
{
    // follow_rate_h 8 matches no preset: the live preset is Custom, the file still says 102. The startup flush writes
    // it at once, without the camera_live() gate.
    MemoryFiles files = shipped({{"follow_rate_h", 8}});
    Session a(files);
    a.start();
    CHECK_EQ(a.s().follow_rate_h, 8.0);
    CHECK_EQ(a.s().preset, 0);
    CHECK_EQ(files.number(SETTINGS_PATH, "preset").value_or(-1), 0.0);
    CHECK_EQ(files.writes[SETTINGS_PATH], 1);
    CHECK(!a.store.flush_due(T0));
    CHECK(!files.content(PENDING_PATH)); // written, then deleted by the flush
    CHECK_EQ(a.store.stamp(), files.mtime(SETTINGS_PATH));
}

TEST(store, startup_without_the_file)
{
    MemoryFiles files;
    files.put(MANIFEST_PATH, fixture("mod/mod_settings.ini"));
    Session a(files);
    a.start();
    CHECK_EQ(files.waits, 10); // 200 ms of retries for a Mod Menu rename
    CHECK(a.events.logged(L"[DWSmoothwalker] smoothwalker.ini not found, using defaults\n", Level::Warning));
    CHECK_EQ(a.s().follow_rate_h, 6.5);
    CHECK_EQ(a.store.shoulder_key(), std::string("V")); // the default keys are bound
    CHECK(a.events.publishes >= 1);
    CHECK(a.events.stored.empty());
    CHECK(!files.content(SETTINGS_PATH)); // no file made, nothing flushed
    CHECK(!files.content(PENDING_PATH));
    CHECK(!a.store.flush_due(T0));
}

TEST(store, startup_rides_out_a_rename)
{
    MemoryFiles files = shipped({{"follow_rate_h", 9}});
    const std::string ini = *files.content(SETTINGS_PATH);
    files.remove(SETTINGS_PATH);
    files.on_wait = [&] {
        if (files.waits == 3) files.put(SETTINGS_PATH, ini);
    };
    Session a(files);
    a.start();
    CHECK_EQ(files.waits, 3);
    CHECK_EQ(a.s().follow_rate_h, 9.0);
    CHECK(!a.events.logged(L"[DWSmoothwalker] smoothwalker.ini not found, using defaults\n", Level::Warning));
}

TEST(store, startup_reads_log_verbose_first)
{
    dw::g_verbose.store(true);
    MemoryFiles off = shipped();
    Session a(off);
    a.start();
    CHECK(!dw::verbose()); // written whatever the file holds

    MemoryFiles on = shipped({{"log_verbose", 1}});
    on.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    Session b(on);
    b.start();
    CHECK(dw::verbose());
    // The scan's verbose lines are gated from the start.
    CHECK(b.events.logged(L"[DWSmoothwalker] preset 201 from the presets folder: Night\n", Level::Verbose));
    CHECK(b.events.logged(L"[DWSmoothwalker] presets: 0 saved slots, 1 from the presets folder\n", Level::Verbose));
    dw::g_verbose.store(false);
}

TEST(store, startup_adds_missing_keys)
{
    MemoryFiles files = shipped();
    std::string ini = *files.content(SETTINGS_PATH);
    for (const char* k : {"focus_fov", "traversal_rotation", "debug_overlay"})
    {
        auto at = ini.find(std::string("\n") + k + " ");
        CHECK(at != std::string::npos);
        ini.erase(at + 1, ini.find('\n', at + 1) - at);
    }
    files.put(SETTINGS_PATH, ini);
    Session a(files);
    a.start();
    CHECK(a.events.logged(L"[DWSmoothwalker] smoothwalker.ini: added 3 missing keys: traversal_rotation, focus_fov, debug_overlay\n"));
    CHECK_EQ(files.writes[SETTINGS_PATH], 1);
    CHECK_EQ(files.number(SETTINGS_PATH, "focus_fov").value_or(-1), 0.0);
    CHECK_EQ(a.store.baseline().at("debug_overlay"), 0.0);
    CHECK_EQ(a.store.stamp(), files.mtime(SETTINGS_PATH));
    CHECK(!a.store.flush_due(T0)); // the added lines hold the live values
    CHECK(!files.content(PENDING_PATH));
}

TEST(store, missing_keys_only_while_the_stamp_is_unchanged)
{
    MemoryFiles files = shipped();
    std::string ini = *files.content(SETTINGS_PATH);
    auto at = ini.find("\nfocus_fov ");
    ini.erase(at + 1, ini.find('\n', at + 1) - at);
    files.put(SETTINGS_PATH, ini);
    // The Mod Menu saves between the parse and the add: the early log_verbose read is the first read, the parse the
    // second.
    int reads = 0;
    files.after_read = [&](const std::string& path) {
        if (path == MemoryFiles::key(SETTINGS_PATH) && ++reads == 2) files.put(SETTINGS_PATH, ini);
    };
    Session a(files);
    a.start();
    files.after_read = nullptr;
    CHECK_EQ(files.writes[SETTINGS_PATH], 0);
    CHECK(files.content(SETTINGS_PATH)->find("\nfocus_fov ") == std::string::npos);
    CHECK(a.store.stamp() != files.mtime(SETTINGS_PATH)); // the poll reloads it next
    a.poll();
    CHECK_EQ(a.store.stamp(), files.mtime(SETTINGS_PATH));
}

TEST(store, missing_keys_write_failure)
{
    MemoryFiles files = shipped();
    std::string ini = *files.content(SETTINGS_PATH);
    auto at = ini.find("\nfocus_fov ");
    ini.erase(at + 1, ini.find('\n', at + 1) - at);
    files.put(SETTINGS_PATH, ini);
    files.failing.insert(MemoryFiles::key(SETTINGS_PATH));
    Session a(files);
    a.start();
    CHECK(a.events.logged(L"[DWSmoothwalker] smoothwalker.ini: could not add 1 missing keys\n", Level::Warning));
}

TEST(store, startup_restores_the_custom_pin)
{
    // The file says Custom over values Balanced matches: Custom was picked, so it stays, and nothing is pending.
    MemoryFiles files = shipped({{"preset", 0}});
    Session a(files);
    a.start();
    CHECK(a.store.custom_pinned());
    CHECK_EQ(a.s().preset, 0);
    CHECK(!a.store.flush_due(T0));
}

TEST(store, startup_prefers_the_file_preset_among_matches)
{
    // Slot 1 holds Balanced's values: the file's preset picks which of the two matches is shown.
    MemoryFiles files = shipped({{"preset", 1}});
    files.put(slot_path(1), slot_file_content(1, "Slot 1", builtin_presets()[1].values));
    Session a(files);
    a.start();
    CHECK_EQ(a.s().preset, 1);
    MemoryFiles other = shipped();
    other.put(slot_path(1), slot_file_content(1, "Slot 1", builtin_presets()[1].values));
    Session b(other);
    b.start();
    CHECK_EQ(b.s().preset, 102);
}

// ------------------------------------------------------------------------------------------------ the poll

TEST(store, poll_without_a_change_does_nothing)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.events.clear();
    a.poll();
    CHECK(a.events.order.empty());
}

TEST(store, poll_keeps_the_live_settings_while_the_file_is_gone)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.remove(SETTINGS_PATH); // mid-rename
    a.events.clear();
    a.poll();
    CHECK(a.events.order.empty());
    CHECK_EQ(a.s().follow_rate_h, 6.5);
}

TEST(store, rule_a_only_changed_keys_apply)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.live([](Settings& s) { s.shoulder_swap = true; }); // N, not yet written back
    a.events.clear();
    files.menu_apply({{"follow_rate_h", 9}});
    a.poll();
    CHECK_EQ(a.s().follow_rate_h, 9.0);
    CHECK(a.s().shoulder_swap); // the live change survives an Apply of another key
    CHECK(a.events.logged(L"[DWSmoothwalker] settings applied\n"));
    CHECK_EQ(a.events.publishes, 1);
    CHECK_EQ(a.store.baseline().at("follow_rate_h"), 9.0);
    CHECK_EQ(a.s().preset, 0); // Balanced no longer matches, and a built-in is read-only: Custom
}

TEST(store, rule_a_values_are_sanitized)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"follow_rate_h", 90}});
    a.poll();
    CHECK_EQ(a.s().follow_rate_h, 30.0);
    CHECK_EQ(a.store.baseline().at("follow_rate_h"), 90.0); // the baseline is the file as written
    CHECK(a.store.flush_due(T0));                           // the clamp is written back
}

TEST(store, rule_a_enabled_moves_only_with_the_file)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.live([](Settings& s) { s.enabled = false; }); // O
    files.menu_apply({{"max_lag_h", 40}});          // an Apply of something else: the file's enabled is still 1
    a.poll();
    CHECK(!a.s().enabled);
    CHECK(a.events.enabled.empty());
    files.menu_apply({{"enabled", 0}}); // now the file's enabled moves (1 to 0)
    a.poll();
    CHECK_EQ(a.events.enabled.size(), 1u);
    CHECK(a.events.enabled.size() == 1 && !a.events.enabled[0]);
    files.menu_apply({{"enabled", 1}});
    a.poll();
    CHECK(a.s().enabled);
    CHECK(a.events.enabled.size() == 2 && a.events.enabled[1]);
}

TEST(store, touched_file_without_new_numbers)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.events.clear();
    files.menu_apply({{"follow_rate_h", 6.5}}); // same number, new write time
    a.poll();
    CHECK_EQ(a.store.stamp(), files.mtime(SETTINGS_PATH));
    CHECK_EQ(a.events.publishes, 0);
    CHECK(!a.events.logged(L"[DWSmoothwalker] settings applied\n"));
}

TEST(store, rule_b_empty_slot_adopts)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.events.clear();
    files.menu_apply({{"preset", 3}, {"max_lag_h", 40}}); // the slider edit of the same Apply goes in too
    a.poll();
    auto saved = files.content(slot_path(3));
    CHECK(saved.has_value());
    auto [name, values] = parse_preset_file(saved.value_or(""));
    CHECK_EQ(name, std::string("Slot 3"));
    CHECK_EQ(values.size(), PRESET_KEYS.size());
    CHECK(!values.empty() && values[6].first == "max_lag_h" && values[6].second == 40.0);
    CHECK_EQ(a.store.loaded_id(), 3);
    CHECK_EQ(a.s().preset, 3);
    CHECK(a.store.find_preset(3) != nullptr);
    CHECK(!a.store.flush_due(T0)); // the file already says preset = 3 with these values
    CHECK(!files.content(PENDING_PATH));
    const std::vector<std::string> order{"log", "banner", "publish", "log"};
    CHECK(a.events.order == order);
    CHECK(a.events.logged(L"[DWSmoothwalker] saved slot 3\n"));
    CHECK(a.events.banners.size() == 1 && a.events.banners[0] == L"Smoothwalker: saved to Slot 3");
}

TEST(store, rule_b_slot_write_failure)
{
    MemoryFiles files = shipped();
    files.failing.insert(MemoryFiles::key(slot_path(3)));
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 3}});
    a.poll();
    CHECK(a.events.logged(L"[DWSmoothwalker] saved slot 3: write failed\n"));
    CHECK(a.events.banners.empty());
    CHECK(a.store.find_preset(3) == nullptr);
    CHECK_EQ(a.s().preset, 102); // back to what matches; the file's 3 is written over later
    CHECK(a.store.flush_due(T0));
}

TEST(store, rule_c_loads_the_picked_preset)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.events.clear();
    files.menu_apply({{"preset", 101}});
    a.poll();
    CHECK_EQ(a.s().follow_rate_h, 12.0);
    CHECK_EQ(a.s().exploration_distance, 90.0);
    CHECK_EQ(a.s().preset, 101);
    CHECK_EQ(a.store.loaded_id(), 101);
    CHECK(a.events.logged(L"[DWSmoothwalker] loaded preset Tight\n"));
    CHECK(a.events.banners.size() == 1 && a.events.banners[0] == L"Smoothwalker: Tight");
    const std::vector<std::string> order{"log", "banner", "publish", "log"};
    CHECK(a.events.order == order);
    // Not written while the page may be open: deferred, mirrored into the side file.
    CHECK_EQ(files.writes[SETTINGS_PATH], 0);
    CHECK(a.store.flush_due(T0));
    CHECK(files.content(PENDING_PATH).has_value());
}

TEST(store, rule_c_edits_in_the_same_apply_win)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 101}, {"follow_rate_h", 9}});
    a.poll();
    CHECK_EQ(a.s().follow_rate_h, 9.0);
    CHECK_EQ(a.s().max_lag_h, 40.0); // Tight's
    CHECK_EQ(a.s().preset, 0);       // Tight no longer matches, and a built-in stays read-only
}

TEST(store, rule_c_on_a_slot_saves_the_edits_back)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 2}}); // adopt Balanced into slot 2
    a.poll();
    files.menu_apply({{"preset", 101}});
    a.poll();
    const int writes = files.writes[MemoryFiles::key(slot_path(2))];
    files.menu_apply({{"preset", 2}, {"max_lag_h", 60}});
    a.poll();
    CHECK_EQ(a.s().max_lag_h, 60.0);
    CHECK_EQ(a.s().follow_rate_h, 6.5); // slot 2's, loaded under the edit
    CHECK_EQ(files.writes[MemoryFiles::key(slot_path(2))], writes + 1);
    CHECK_EQ(files.number(slot_path(2), "max_lag_h").value_or(-1), 60.0);
    CHECK_EQ(a.s().preset, 2);
    CHECK(a.events.logged(L"[DWSmoothwalker] loaded preset Slot 2\n"));
    CHECK(a.events.logged(L"[DWSmoothwalker] saved slot 2\n"));
}

TEST(store, rule_c_unknown_preset)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 250}});
    a.poll();
    CHECK(a.events.logged(L"[DWSmoothwalker] preset 250 is empty, nothing loaded\n", Level::Warning));
    CHECK_EQ(a.s().preset, 102);
    CHECK(a.store.flush_due(T0)); // 102 is written over the 250
}

TEST(store, rule_d_custom_pins)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 1}}); // adopt into slot 1
    a.poll();
    files.menu_apply({{"preset", 0}});
    a.poll();
    CHECK(a.store.custom_pinned());
    CHECK_EQ(a.store.loaded_id(), 0);
    CHECK_EQ(a.s().preset, 0); // the values still match slot 1 and Balanced: pinned anyway
    const int writes = files.writes[MemoryFiles::key(slot_path(1))];
    files.menu_apply({{"max_lag_h", 70}});
    a.poll();
    CHECK_EQ(files.writes[MemoryFiles::key(slot_path(1))], writes); // not saved into the slot it came from
    CHECK_EQ(a.s().preset, 0);
    // The pin holds until a preset is loaded.
    a.cycle();
    CHECK(!a.store.custom_pinned());
    CHECK_EQ(a.s().preset, 101);
}

TEST(store, rule_d_pin_ends_with_an_adopt)
{
    MemoryFiles files = shipped({{"preset", 0}});
    Session a(files);
    a.start();
    CHECK(a.store.custom_pinned());
    files.menu_apply({{"preset", 4}});
    a.poll();
    CHECK(!a.store.custom_pinned());
    CHECK_EQ(a.s().preset, 4);
}

TEST(store, rule_e_autosaves_the_active_slot)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 2}});
    a.poll();
    a.events.clear();
    files.menu_apply({{"max_lag_h", 40}});
    a.poll();
    CHECK_EQ(files.number(slot_path(2), "max_lag_h").value_or(-1), 40.0);
    CHECK_EQ(a.s().preset, 2);
    CHECK_EQ(a.store.loaded_id(), 2);
    CHECK(!a.store.flush_due(T0)); // the file already holds what the slot now holds
    const std::vector<std::string> order{"log", "banner", "publish", "log"};
    CHECK(a.events.order == order);
    CHECK(a.events.banners.size() == 1 && a.events.banners[0] == L"Smoothwalker: Slot 2 updated");
}

TEST(store, rule_e_needs_a_preset_key)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 2}});
    a.poll();
    const int writes = files.writes[MemoryFiles::key(slot_path(2))];
    files.menu_apply({{"show_banner", 0}, {"reset_gap", 0.5}}); // not preset keys
    a.poll();
    CHECK_EQ(files.writes[MemoryFiles::key(slot_path(2))], writes);
    CHECK_EQ(a.s().preset, 2);
}

TEST(store, rule_e_builtins_are_read_only)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"max_lag_h", 40}}); // Balanced active
    a.poll();
    CHECK_EQ(a.s().preset, 0);
    CHECK(files.list(PRESETS_DIR_W).slots.empty()); // nothing saved
    CHECK(a.events.banners.empty());
}

TEST(store, exactly_one_rule_per_apply)
{
    // Slot 2 active, an Apply that picks Tight and moves a preset key: the load (c), not an autosave into slot 2 (e).
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 2}});
    a.poll();
    const int writes = files.writes[MemoryFiles::key(slot_path(2))];
    files.menu_apply({{"preset", 101}, {"max_lag_h", 60}});
    a.poll();
    CHECK_EQ(files.writes[MemoryFiles::key(slot_path(2))], writes);
    CHECK_EQ(files.number(slot_path(2), "max_lag_h").value_or(-1), 85.0);
    CHECK_EQ(a.s().max_lag_h, 60.0);
    CHECK_EQ(a.s().follow_rate_h, 12.0);
}

// ------------------------------------------------------------------------------------------ presets and slots

TEST(store, slots_and_dropins_scanned_once)
{
    MemoryFiles files = shipped();
    files.put(slot_path(1), "name = Evening\nfollow_rate_h = 3\ncurve_h = 1.4\n");
    files.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    files.put(dropin_path("alpha.ini"), "name = A|lpha\nmax_lag_h = 999\n");
    files.put(dropin_path("Empty.ini"), "; nothing\nenabled = 0\n");
    files.put(dropin_path("Big.ini"), "follow_rate_h = 4\n" + std::string(64 * 1024, ';'));
    Session a(files);
    a.start();
    CHECK_EQ(a.store.loaded_slots(), 1u);
    CHECK_EQ(a.store.loaded_dropins(), 2u);
    const auto& p = a.store.presets();
    CHECK_EQ(p.size(), 6u);
    if (p.size() == 6)
    {
        CHECK_EQ(p[3].id, 1);
        CHECK_EQ(p[3].name, std::string("Evening"));
        CHECK_EQ(p[3].values[1].first, std::string("curve_h"));
        CHECK_EQ(p[3].values[1].second, 1.0); // normalized: rounded
        CHECK_EQ(p[4].id, 201);
        CHECK_EQ(p[4].name, std::string("Alpha")); // '|' stripped; sorted case-insensitively
        CHECK_EQ(p[4].values[0].second, 300.0);    // clamped
        CHECK_EQ(p[5].id, 202);
        CHECK_EQ(p[5].name, std::string("Night")); // the file stem
    }
    CHECK(a.events.logged(L"[DWSmoothwalker] presets/Empty.ini: no preset settings, skipped\n", Level::Warning));
    CHECK(a.events.logged(L"[DWSmoothwalker] presets/Big.ini: unreadable or over 64 KiB, skipped\n", Level::Warning));
    CHECK_EQ(manifest_line(files, "PresetValues"), std::string("0|101|102|103|1|201|202|2|3|4|5|6"));
    CHECK_EQ(manifest_line(files, "PresetLabels"),
             std::string("Custom|Tight|Balanced|Cinematic|Evening|Alpha|Night|Slot 2 (empty)|Slot 3 (empty)|Slot 4 (empty)|Slot 5 (empty)|Slot 6 (empty)"));
    CHECK_EQ(files.writes[MANIFEST_PATH], 1);
}

TEST(store, dropins_off_without_a_manifest)
{
    MemoryFiles files = shipped();
    files.remove(MANIFEST_PATH);
    files.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    Session a(files);
    a.start();
    CHECK(a.events.logged(L"[DWSmoothwalker] mod_settings.ini not found\n", Level::Warning));
    CHECK(a.events.logged(L"[DWSmoothwalker] 1 presets from the presets folder off for this session: the Mod Menu page could not list them\n",
                          Level::Warning));
    CHECK_EQ(a.store.loaded_dropins(), 0u);
    CHECK(a.store.find_preset(201) == nullptr);
}

TEST(store, dropins_off_when_the_manifest_cannot_list_them)
{
    MemoryFiles files = shipped();
    files.put(MANIFEST_PATH, "[Setting.preset]\nPresetValues = 0\n");
    files.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    Session a(files);
    a.start();
    CHECK(a.events.logged(L"[DWSmoothwalker] mod_settings.ini: [Setting.preset] PresetValues or PresetLabels missing\n", Level::Warning));
    CHECK_EQ(a.store.loaded_dropins(), 0u);

    MemoryFiles failing = shipped();
    failing.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    failing.failing.insert(MemoryFiles::key(MANIFEST_PATH));
    Session b(failing);
    b.start();
    CHECK(b.events.logged(L"[DWSmoothwalker] could not write mod_settings.ini\n", Level::Warning));
    CHECK_EQ(b.store.loaded_dropins(), 0u);
}

TEST(store, dropin_limit)
{
    MemoryFiles files = shipped();
    for (int i = 0; i < MAX_DROPINS + 2; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "p%03d.ini", i);
        files.put(dropin_path(name), "follow_rate_h = 4\n");
    }
    Session a(files);
    a.start();
    CHECK_EQ(a.store.loaded_dropins(), static_cast<size_t>(MAX_DROPINS));
    CHECK(a.events.logged(L"[DWSmoothwalker] 2 presets over the limit of 54 skipped\n", Level::Warning));
}

TEST(store, slot_names_are_kept)
{
    MemoryFiles files = shipped({{"preset", 1}});
    files.put(slot_path(1), slot_file_content(1, "Evening", builtin_presets()[1].values));
    Session a(files);
    a.start();
    CHECK_EQ(a.s().preset, 1);
    files.menu_apply({{"max_lag_h", 40}});
    a.poll();
    CHECK(a.events.banners.size() == 1 && a.events.banners[0] == L"Smoothwalker: Evening updated");
    auto [name, values] = parse_preset_file(files.content(slot_path(1)).value_or(""));
    CHECK_EQ(name, std::string("Evening")); // a renamed slot keeps its name on re-save
}

TEST(store, a_new_slot_takes_its_place_in_the_cycle)
{
    MemoryFiles files = shipped();
    files.put(slot_path(1), slot_file_content(1, "One", builtin_presets()[0].values));
    files.put(slot_path(4), slot_file_content(4, "Four", builtin_presets()[2].values));
    files.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 3}, {"max_lag_h", 33}});
    a.poll();
    std::vector<int> ids;
    for (auto& p : a.store.presets()) ids.push_back(p.id);
    const std::vector<int> expect{101, 102, 103, 1, 3, 4, 201};
    CHECK(ids == expect);
}

TEST(store, cycle_order)
{
    MemoryFiles files = shipped();
    files.put(slot_path(2), slot_file_content(2, "Two", {{"follow_rate_h", 3}}));
    files.put(dropin_path("Night.ini"), "follow_rate_h = 4\n");
    Session a(files);
    a.start();
    std::vector<int> seen;
    for (int i = 0; i < 6; ++i)
    {
        a.cycle();
        seen.push_back(a.s().preset);
    }
    // From Balanced: Cinematic, slot 2, the drop-in, then round to Tight. Two and Night hold only follow_rate_h and
    // match on it alone.
    const std::vector<int> expect{103, 2, 201, 101, 102, 103};
    CHECK(seen == expect);
    CHECK(a.events.logged(L"[DWSmoothwalker] loaded preset Two\n"));
    CHECK(a.events.logged(L"[DWSmoothwalker] loaded preset Night\n"));
    CHECK(a.store.flush_due(T0));
}

TEST(store, cycle_from_custom_starts_at_the_first)
{
    MemoryFiles files = shipped({{"follow_rate_h", 8}});
    Session a(files);
    a.start();
    CHECK_EQ(a.s().preset, 0);
    a.events.clear();
    a.cycle();
    CHECK_EQ(a.s().preset, 101);
    const std::vector<std::string> order{"log", "banner", "publish"};
    CHECK(a.events.order == order);
}

// --------------------------------------------------------------------------------- write-back and side file

TEST(store, flush_writes_the_differing_numbers)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 103}});
    a.poll();
    const std::string before = *files.content(SETTINGS_PATH);
    a.flush();
    const std::string after = *files.content(SETTINGS_PATH);
    CHECK(after != before);
    CHECK_EQ(files.number(SETTINGS_PATH, "follow_rate_h").value_or(-1), 4.0);
    CHECK_EQ(files.number(SETTINGS_PATH, "preset").value_or(-1), 103.0);
    CHECK(after.find("follow_rate_h    = 4   ; horizontal catch-up rate") != std::string::npos); // in place
    CHECK_EQ(a.store.stamp(), files.mtime(SETTINGS_PATH));
    CHECK(!a.store.flush_due(T0));
    CHECK(!files.content(PENDING_PATH));
    CHECK(a.store.pending_file().empty());
    a.events.clear();
    a.poll(); // our own write: the stamp matches, no reload
    CHECK(a.events.order.empty());
}

TEST(store, flush_waits_for_the_poll)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.live([](Settings& s) { s.shoulder_swap = true; });
    files.menu_apply({{"max_lag_h", 40}}); // not polled yet
    const int writes = files.writes[SETTINGS_PATH];
    a.flush(T0);
    CHECK_EQ(files.writes[SETTINGS_PATH], writes);
    CHECK(!a.store.flush_due(T0 + milliseconds(249))); // throttled
    CHECK(a.store.flush_due(T0 + milliseconds(250)));
    a.poll();
    a.flush(T0 + milliseconds(250));
    CHECK_EQ(files.number(SETTINGS_PATH, "shoulder_swap").value_or(-1), 1.0);
    CHECK_EQ(files.number(SETTINGS_PATH, "max_lag_h").value_or(-1), 40.0); // the menu's edit kept
}

TEST(store, flush_retries_and_warns_once)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    a.live([](Settings& s) { s.shoulder_swap = true; });
    files.failing.insert(MemoryFiles::key(SETTINGS_PATH));
    a.flush(T0);
    a.flush(T0 + milliseconds(250));
    size_t warnings = 0;
    for (auto& [level, line] : a.events.lines)
        if (level == Level::Warning && line == L"[DWSmoothwalker] could not write smoothwalker.ini, retrying\n") ++warnings;
    CHECK_EQ(warnings, 1u);
    CHECK(a.store.flush_failing());
    CHECK(!a.store.flush_due(T0 + milliseconds(499)));
    CHECK(a.store.flush_due(T0 + milliseconds(500)));
    files.failing.clear();
    a.flush(T0 + milliseconds(500));
    CHECK(!a.store.flush_failing());
    CHECK(!a.store.flush_due(T0 + milliseconds(500)));
    CHECK_EQ(files.number(SETTINGS_PATH, "shoulder_swap").value_or(-1), 1.0);
}

TEST(store, flush_counts_a_missing_line_as_written)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    std::string ini = *files.content(SETTINGS_PATH);
    auto at = ini.find("\ndebug_overlay ");
    ini.erase(at + 1, ini.find('\n', at + 1) - at);
    files.put(SETTINGS_PATH, ini);
    a.poll();
    a.live([](Settings& s) { s.debug_overlay = true; });
    const int writes = files.writes[SETTINGS_PATH];
    a.flush();
    CHECK_EQ(files.writes[SETTINGS_PATH], writes); // nothing to rewrite
    CHECK(!a.store.flush_due(T0));                 // and not retried every tick
}

TEST(store, pending_side_file)
{
    MemoryFiles files = shipped();
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 101}});
    a.poll();
    const std::string pending = files.content(PENDING_PATH).value_or("");
    const std::string head = "; DWSmoothwalker: settings not yet written to smoothwalker.ini. Applied at the next start only while\n"
                             "; smoothwalker.ini's write time still equals stamp.\n"
                             "stamp = " +
                             std::to_string(files.mtime(SETTINGS_PATH)) + "\n";
    CHECK(pending.starts_with(head));
    CHECK(pending.find("\nfollow_rate_h = 12\n") != std::string::npos);
    CHECK(pending.find("\npreset = ") == std::string::npos); // the Apply wrote preset = 101 itself
    CHECK(pending.find("\nenabled = ") == std::string::npos); // only what differs
    CHECK_EQ(a.store.pending_file(), pending);
    // Rewritten only when its content changes.
    const int writes = files.writes[PENDING_PATH];
    {
        std::lock_guard guard(a.store.mutex());
        a.store.mark_pending_locked();
    }
    CHECK_EQ(files.writes[PENDING_PATH], writes);
}

TEST(store, pending_file_applies_at_the_next_start)
{
    // A preset loaded from the paused menu, then the game exits before the camera is live: the next start applies it.
    MemoryFiles files = shipped();
    {
        Session a(files);
        a.start();
        files.menu_apply({{"preset", 103}});
        a.poll();
        CHECK(files.content(PENDING_PATH).has_value());
    }
    Session b(files);
    b.start();
    CHECK(b.events.logged(L"[DWSmoothwalker] applied 23 settings the last session had not written yet\n"));
    CHECK_EQ(b.s().follow_rate_h, 4.0);
    CHECK_EQ(b.s().preset, 103);
    CHECK_EQ(files.number(SETTINGS_PATH, "follow_rate_h").value_or(-1), 4.0); // the startup flush wrote it
    CHECK(!files.content(PENDING_PATH));
}

TEST(store, pending_file_ignored_after_a_change)
{
    MemoryFiles files = shipped();
    {
        Session a(files);
        a.start();
        files.menu_apply({{"preset", 103}});
        a.poll();
    }
    files.menu_apply({{"max_lag_v", 30}}); // the file changed since the side file's stamp
    Session b(files);
    b.start();
    CHECK(b.events.logged(L"[DWSmoothwalker] smoothwalker.pending ignored: smoothwalker.ini changed since\n"));
    CHECK_EQ(b.s().follow_rate_h, 6.5); // the file's, not the side file's
    CHECK_EQ(b.s().max_lag_v, 30.0);
    CHECK(!files.content(PENDING_PATH)); // deleted either way
}

TEST(store, pending_write_failure)
{
    MemoryFiles files = shipped();
    files.failing.insert(MemoryFiles::key(PENDING_PATH));
    Session a(files);
    a.start();
    files.menu_apply({{"preset", 101}});
    a.poll();
    CHECK(a.events.logged(L"[DWSmoothwalker] could not write smoothwalker.pending\n", Level::Warning));
    CHECK(a.store.pending_file().empty());
    CHECK(a.store.flush_due(T0)); // the write-back itself is still pending
}
