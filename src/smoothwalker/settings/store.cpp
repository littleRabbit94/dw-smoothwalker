// The settings file state machine (settings/store.hpp). Bodies moved from Smoothwalker::Impl unchanged but for the
// seams: file access through m_files, log lines, banners, publishes and the switch through m_events.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "store.hpp"
#include "ini.hpp"
#include "../../common/log.hpp"
#include "../../common/text.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dw::smoothwalker::settings
{
namespace
{
    auto widen(const std::string& s) -> std::wstring
    {
        return std::wstring(s.begin(), s.end());
    }
} // namespace

    // ---------------------------------------------------------------------------------------------- Win32Files

    auto Win32Files::read(const std::string& path) -> std::optional<std::string> { return read_file(path); }
    auto Win32Files::read_small(const std::wstring& path, size_t limit) -> std::optional<std::string> { return read_small_file(path, limit); }
    auto Win32Files::write(const std::string& path, const std::string& content) -> bool { return write_file(path, content); }

    auto Win32Files::mtime(const std::string& path) -> uint64_t
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
        return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }

    auto Win32Files::remove(const std::string& path) -> void { DeleteFileA(path.c_str()); }
    auto Win32Files::mkdir(const std::string& path) -> void { CreateDirectoryA(path.c_str(), nullptr); }
    auto Win32Files::mkdir(const std::wstring& path) -> void { CreateDirectoryW(path.c_str(), nullptr); }
    auto Win32Files::list(const std::wstring& dir) -> PresetFiles { return list_preset_files(dir); }
    auto Win32Files::wait(int milliseconds) -> void { Sleep(static_cast<DWORD>(milliseconds)); }

    // ------------------------------------------------------------------------------------------- SettingsStore

    auto SettingsStore::log(Level level, const std::wstring& line) -> void
    {
        m_events.log(level, line);
    }

    auto SettingsStore::start_locked() -> void
    {
        // log_verbose is read once here, ahead of load_presets_locked(), so its per-preset lines are gated from
        // the start; reload_settings_locked(true) below re-derives the same value from the full parse. Written
        // whatever the file holds (off without the key or the file): the flag is image-level (common/log.hpp), and
        // this write, not an unload reset, is what starts each instance from its own ini.
        bool verbose = false;
        if (auto content = m_files.read(SETTINGS_PATH))
        {
            auto numbers = parse_numbers(*content);
            if (auto found = numbers.find("log_verbose"); found != numbers.end()) verbose = found->second != 0.0;
        }
        dw::g_verbose.store(verbose);
        load_presets_locked();
        reload_settings_locked(true);
        add_missing_keys_locked();
        // Once, without the camera_live() gate: no Mod Menu page can be open this early (docs/design.md, "Startup
        // flush"). Fixes a preset id the regenerated manifest may not list yet, before the page can fail on it.
        if (m_flush_pending.load()) flush_locked(std::chrono::steady_clock::now());
    }

    auto SettingsStore::poll_locked() -> void
    {
        if (m_files.mtime(SETTINGS_PATH) != m_settings_stamp) reload_settings_locked(false);
    }

    // Startup parses every value; a later change applies only keys whose number moved since the file was last
    // seen, so a live key or preset change survives an Apply of the rest. Only flush_locked writes smoothwalker.ini.
    auto SettingsStore::reload_settings_locked(bool startup) -> void
    {
        // Stamp first: a write landing between the two is then seen by the next poll.
        auto stamp = m_files.mtime(SETTINGS_PATH);
        auto content = m_files.read(SETTINGS_PATH);
        // At startup the key names are read only once, so ride out a Mod Menu rename in progress.
        for (int i = 0; startup && !content && i < 10; ++i)
        {
            m_files.wait(20);
            stamp = m_files.mtime(SETTINGS_PATH);
            content = m_files.read(SETTINGS_PATH);
        }
        if (!content)
        {
            // The Mod Menu replaces the file by rename, so it is briefly absent: keep what is live and retry.
            if (!startup) return;
            log(Level::Warning, std::format(L"[DWSmoothwalker] smoothwalker.ini not found, using defaults\n"));
            m_settings = Settings{};
            m_toggle_key = m_settings.toggle_key;
            m_preset_key = m_settings.preset_key;
            m_shoulder_key = m_settings.shoulder_key;
            m_debug_key = m_settings.debug_key;
            m_events.publish();
            return;
        }
        m_settings_stamp = stamp;
        auto file = parse_numbers(*content);

        if (startup)
        {
            m_settings = parse_settings(*content);
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
            m_events.publish();
            m_events.store_enabled(m_settings.enabled);
            mark_pending_locked();
            return;
        }

        Values edits;
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
        auto is_slot = [](int id) { return id >= 1 && id <= MAX_SLOTS; };
        int active_before = m_settings.preset; // the slot an Apply that leaves the picker alone saves into

        // (a) Ordinary edits onto the live settings. The toggle's state changes only if the file's enabled did.
        apply_values(m_settings, edits);
        if (edited("enabled")) m_events.set_enabled(m_settings.enabled); // fades like the toggle key

        Values own; // the preset keys this Apply moved
        for (auto& edit : edits)
        {
            if (is_preset_key(edit.first)) own.push_back(edit);
        }
        auto slot_name = [&](int id) {
            auto* slot = find_preset(id);
            return dw::to_wide(slot ? slot->name : "Slot " + std::to_string(id));
        };
        // The slot file is written now: the menu does not watch it.
        auto save_slot = [&](int id) {
            bool ok = save_slot_locked(id, preset_of(m_settings));
            log(Level::Normal, std::format(L"[DWSmoothwalker] saved slot {}{}\n", id, ok ? L"" : L": write failed"));
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
                m_events.banner(L"Smoothwalker: saved to " + slot_name(picked));
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
                apply_values(m_settings, own);
                if (is_slot(picked) && !own.empty()) save_slot(picked);
            }
        }
        else if (edited("preset"))
        {
            // (d) Custom: detached from whatever was active, nothing saved. Pinned, or the values would match the slot
            // again and the next edit would be saved into it.
            m_loaded_id = 0;
            m_custom_pinned = true;
        }
        else if (is_slot(active_before) && !own.empty())
        {
            // (e) An edit with a slot active is saved into it. Built-ins and drop-ins stay read-only: an edit
            // there just falls through to update_active_locked, which gives Custom.
            if (save_slot(active_before))
            {
                m_loaded_id = active_before;
                m_events.banner(L"Smoothwalker: " + slot_name(active_before) + L" updated");
            }
        }

        update_active_locked();
        m_events.publish();
        mark_pending_locked();
        log(Level::Normal, std::format(L"[DWSmoothwalker] settings applied\n"));
    }

    // Once per session, in the constructor, before anything reads presets or the Mod Menu reads mod_settings.ini:
    // built-ins, slots 1..MAX_SLOTS ("Slot N.ini"), then other presets-folder *.ini as drop-ins (201+, by name).
    // On a failed manifest rewrite drop-ins stay off, so the indicator never shows an id the page lacks.
    auto SettingsStore::load_presets_locked() -> void
    {
        m_presets.clear();
        for (auto& p : builtin_presets()) m_presets.push_back({p.id, p.name, p.values});
        m_files.mkdir(std::wstring(PRESETS_DIR_W)); // so it is there to drop files into; fails harmlessly if present

        auto files = m_files.list(PRESETS_DIR_W);
        auto read_preset = [&](const std::wstring& name) -> std::optional<std::pair<std::string, Values>> {
            auto content = m_files.read_small(std::wstring(PRESETS_DIR_W) + L"\\" + name, MAX_PRESET_FILE);
            if (!content)
            {
                log(Level::Warning, std::format(L"[DWSmoothwalker] presets/{}: unreadable or over 64 KiB, skipped\n", name));
                return std::nullopt;
            }
            auto parsed = parse_preset_file(std::move(*content));
            if (parsed.second.empty())
            {
                log(Level::Warning, std::format(L"[DWSmoothwalker] presets/{}: no preset settings, skipped\n", name));
                return std::nullopt;
            }
            return parsed;
        };

        for (auto& [slot, name] : files.slots)
        {
            // The file's name line is the slot's display name; the file name stays "Slot N.ini".
            if (auto parsed = read_preset(name))
            {
                m_presets.push_back({slot, display_name(parsed->first, "Slot " + std::to_string(slot), slot), normalize_preset(parsed->second)});
            }
        }

        std::vector<Preset> dropins;
        size_t over_limit = 0;
        for (auto& name : files.dropins)
        {
            if (dropins.size() >= static_cast<size_t>(MAX_DROPINS))
            {
                ++over_limit;
                continue;
            }
            auto parsed = read_preset(name);
            if (!parsed) continue;
            int id = FIRST_DROPIN_ID + static_cast<int>(dropins.size());
            auto stem = dw::utf8_of(name.substr(0, name.size() - 4)).value_or("");
            dropins.push_back({id, display_name(parsed->first, stem, id), normalize_preset(parsed->second)});
        }
        if (over_limit)
        {
            log(Level::Warning, std::format(L"[DWSmoothwalker] {} presets over the limit of {} skipped\n", over_limit, MAX_DROPINS));
        }

        // Picker order: saved slots, drop-ins, then the empty slots, so nothing empty sits between the presets that
        // load. Empty slots stay listed: the page fails to open if the ini's preset id is not among the values, and
        // a slot saved this session becomes the active id.
        std::string values = "0|101|102|103", labels = "Custom|Tight|Balanced|Cinematic";
        std::string empty_values, empty_labels;
        for (int n = 1; n <= MAX_SLOTS; ++n)
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
        if (auto manifest = m_files.read(MANIFEST_PATH))
        {
            if (auto updated = with_preset_choices(*manifest, "[Setting.preset]", values, labels))
            {
                listed = *updated == *manifest || m_files.write(MANIFEST_PATH, *updated);
                if (!listed) log(Level::Warning, std::format(L"[DWSmoothwalker] could not write mod_settings.ini\n"));
            }
            else
            {
                log(Level::Warning, std::format(L"[DWSmoothwalker] mod_settings.ini: [Setting.preset] PresetValues or PresetLabels missing\n"));
            }
        }
        else
        {
            log(Level::Warning, std::format(L"[DWSmoothwalker] mod_settings.ini not found\n"));
        }
        if (!listed && !dropins.empty())
        {
            log(Level::Warning, std::format(L"[DWSmoothwalker] {} presets from the presets folder off for this session: the Mod Menu page could not list them\n",
                                            dropins.size()));
            dropins.clear();
        }
        bool verbose = dw::verbose();
        if (verbose)
        {
            for (auto& p : dropins)
            {
                log(Level::Verbose, std::format(L"[DWSmoothwalker] preset {} from the presets folder: {}\n", p.id, dw::to_wide(p.name)));
            }
        }
        size_t slots = m_presets.size() - builtin_presets().size();
        m_presets.insert(m_presets.end(), std::make_move_iterator(dropins.begin()), std::make_move_iterator(dropins.end()));
        m_loaded_slots = slots;
        m_loaded_dropins = m_presets.size() - slots - builtin_presets().size();
        if (verbose)
        {
            log(Level::Verbose, std::format(L"[DWSmoothwalker] presets: {} saved slots, {} from the presets folder\n", m_loaded_slots, m_loaded_dropins));
        }
    }

    auto SettingsStore::save_slot_locked(int slot, Values values) -> bool
    {
        values = normalize_preset(values);
        m_files.mkdir(std::string(PRESETS_DIR));
        auto path = std::string(PRESETS_DIR) + "/Slot " + std::to_string(slot) + ".ini";
        auto* known = find_preset(slot);
        auto name = known ? known->name : "Slot " + std::to_string(slot); // a renamed slot keeps its name
        if (!m_files.write(path, slot_file_content(slot, name, values))) return false;
        if (known)
        {
            known->values = std::move(values);
            return true;
        }
        auto is_after = [&](const Preset& p) { return (p.id >= 1 && p.id <= MAX_SLOTS && p.id > slot) || p.id >= FIRST_DROPIN_ID; };
        m_presets.insert(std::find_if(m_presets.begin(), m_presets.end(), is_after), Preset{slot, "Slot " + std::to_string(slot), std::move(values)});
        return true;
    }

    auto SettingsStore::find_preset(int id) -> Preset*
    {
        auto it = std::find_if(m_presets.begin(), m_presets.end(), [&](const Preset& p) { return p.id == id; });
        return it != m_presets.end() ? &*it : nullptr;
    }

    // Applies only the keys the preset holds; a hand-edited file may hold fewer.
    auto SettingsStore::load_preset_locked(int id) -> bool
    {
        auto* preset = find_preset(id);
        if (!preset)
        {
            log(Level::Warning, std::format(L"[DWSmoothwalker] preset {} is empty, nothing loaded\n", id));
            return false;
        }
        apply_values(m_settings, preset->values);
        m_loaded_id = id;
        m_custom_pinned = false;
        auto name = dw::to_wide(preset->name);
        log(Level::Normal, std::format(L"[DWSmoothwalker] loaded preset {}\n", name));
        m_events.banner(L"Smoothwalker: " + name);
        return true;
    }

    // m_settings.preset becomes the preset the live settings match, preferring the last one loaded, else 0 (Custom).
    auto SettingsStore::update_active_locked() -> void
    {
        auto matches = [&](const Preset& p) {
            return !p.values.empty() && std::all_of(p.values.begin(), p.values.end(), [&](auto& entry) {
                return std::abs(number_of(m_settings, entry.first) - entry.second) <= 1e-4;
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
    auto SettingsStore::cycle_locked() -> void
    {
        auto at = std::find_if(m_presets.begin(), m_presets.end(), [&](const Preset& p) { return p.id == m_settings.preset; });
        size_t next = m_settings.preset == 0 || at == m_presets.end() ? 0 : (static_cast<size_t>(at - m_presets.begin()) + 1) % m_presets.size();
        if (!load_preset_locked(m_presets[next].id)) return;
        update_active_locked();
        m_events.publish();
        mark_pending_locked();
    }

    // The live numbers that differ from the file; preset is written as m_settings holds it (the active preset).
    // Compared as written (%.6g), so a value the file cannot hold exactly is not rewritten forever.
    auto SettingsStore::pending_writes_locked() -> Values
    {
        Values writes;
        for (auto* key : NUMERIC_KEYS)
        {
            double value = number_of(m_settings, key);
            auto known = m_baseline.find(key);
            if (known == m_baseline.end() || format_number(known->second) != format_number(value)) writes.emplace_back(key, value);
        }
        return writes;
    }

    // After any change to the live settings, the baseline or the stamp. Marks the write-back and mirrors it into
    // smoothwalker.pending (which the Mod Menu does not read), so a preset loaded from a paused menu survives an exit
    // or hot reload before the camera goes live. The side file is rewritten only when its content changes.
    auto SettingsStore::mark_pending_locked() -> void
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
            m_files.remove(PENDING_PATH);
        }
        else if (!m_files.write(PENDING_PATH, content))
        {
            log(Level::Warning, std::format(L"[DWSmoothwalker] could not write smoothwalker.pending\n"));
            return;
        }
        m_pending_file = std::move(content);
    }

    // Once, in the constructor after the full parse, so the Mod Menu never reads a file without them: an ini copied
    // back from an older version lacks the newer keys, and the page opens only if every ConfigKey is in the file.
    // The added lines hold the live values, so the baseline takes them as written, as the flush does.
    auto SettingsStore::add_missing_keys_locked() -> void
    {
        if (m_files.mtime(SETTINGS_PATH) != m_settings_stamp) return; // missing (stamp 0 both), or changed since the parse
        auto content = m_files.read(SETTINGS_PATH);
        if (!content) return;
        auto [updated, added] = with_missing_keys(*content, m_settings);
        if (added.empty()) return;
        if (!m_files.write(SETTINGS_PATH, updated))
        {
            log(Level::Warning, std::format(L"[DWSmoothwalker] smoothwalker.ini: could not add {} missing keys\n", added.size()));
            return;
        }
        m_settings_stamp = m_files.mtime(SETTINGS_PATH);
        std::string names;
        for (auto& key : added)
        {
            if (is_numeric_key(key)) m_baseline[key] = std::stod(format_number(number_of(m_settings, key))); // debug_key is a name
            names += (names.empty() ? "" : ", ") + key;
        }
        mark_pending_locked(); // the added keys are no longer pending, and the side file takes the new stamp
        log(Level::Normal, std::format(L"[DWSmoothwalker] smoothwalker.ini: added {} missing keys: {}\n", added.size(), widen(names)));
    }

    // A write-back the last session missed. Applied only if smoothwalker.ini still has the stamped write time.
    auto SettingsStore::apply_pending_file_locked() -> void
    {
        auto content = m_files.read(PENDING_PATH);
        if (!content) return;
        m_files.remove(PENDING_PATH);

        std::optional<uint64_t> stamp;
        std::istringstream in(*content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos || trim(line.substr(0, eq)) != "stamp") continue;
            try
            {
                stamp = std::stoull(trim(line.substr(eq + 1)));
            }
            catch (...)
            {
            }
        }
        if (!stamp || *stamp != m_settings_stamp)
        {
            log(Level::Normal, std::format(L"[DWSmoothwalker] smoothwalker.pending ignored: smoothwalker.ini changed since\n"));
            return;
        }
        auto numbers = parse_numbers(*content);
        apply_values(m_settings, Values(numbers.begin(), numbers.end()));
        log(Level::Normal, std::format(L"[DWSmoothwalker] applied {} settings the last session had not written yet\n", numbers.size()));
    }

    // Runs only while the camera is live, so no Mod Menu page is open to refuse its next Apply over the change.
    auto SettingsStore::flush_locked(std::chrono::steady_clock::time_point now) -> void
    {
        // The file changed since last read (or is gone): let the poll handle it first, throttled so a deleted
        // file is not queried every tick.
        if (m_files.mtime(SETTINGS_PATH) != m_settings_stamp)
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

        auto content = m_files.read(SETTINGS_PATH);
        auto updated = content ? rewrite_numbers(*content, writes) : std::string{};
        bool changed = content && updated != *content;
        if (!content || (changed && !m_files.write(SETTINGS_PATH, updated)))
        {
            if (!m_flush_failing) log(Level::Warning, std::format(L"[DWSmoothwalker] could not write smoothwalker.ini, retrying\n"));
            m_flush_failing = true;
            m_next_flush = now + std::chrono::milliseconds(250);
            return;
        }
        if (changed) m_settings_stamp = m_files.mtime(SETTINGS_PATH);
        // A key missing from the file counts as written too, so it is not retried every tick.
        for (auto& [key, value] : writes) m_baseline[key] = std::stod(format_number(value));
        m_flush_failing = false;
        mark_pending_locked(); // nothing left: clears the flag and deletes smoothwalker.pending
    }
} // namespace dw::smoothwalker::settings
