// Presets (settings/presets.hpp).
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "presets.hpp"
#include "ini.hpp"
#include "../../common/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dw::smoothwalker::settings
{
    auto builtin_presets() -> const std::vector<NamedPreset>&
    {
        static const std::vector<NamedPreset> presets{
                {101, "Tight", {{"follow_rate_h", 12}, {"follow_rate_v", 20}, {"curve_h", 0}, {"curve_v", 0}, {"catchup_distance", 100},
                                {"min_rate_scale", 0.5}, {"max_lag_h", 40}, {"max_lag_v", 20}, {"soft_leash", 1},
                                {"aiming_follow", 30}, {"combat_follow", 100}, {"traversal_follow", 100},
                                {"rotation_smoothing", 0}, {"rotation_rate", 20}, {"combat_rotation", 100}, {"traversal_rotation", 100},
                                {"pitch_min", -60}, {"pitch_max", 40},
                                {"exploration_distance", 90}, {"exploration_height", 0}, {"exploration_shoulder", 0}, {"exploration_fov", 0},
                                {"sprint_distance", 90}, {"sprint_height", 0}, {"sprint_shoulder", 0}, {"sprint_fov", 0},
                                {"combat_distance", 95}, {"combat_height", 0}, {"combat_shoulder", 0}, {"combat_fov", 0},
                                {"focus_distance", 95}, {"focus_height", 0}, {"focus_shoulder", 0}, {"focus_fov", 0},
                                {"aiming_distance", 100}, {"aiming_height", 0}, {"aiming_shoulder", 0}, {"aiming_fov", 0},
                                {"traversal_distance", 95}, {"traversal_height", 0}, {"traversal_fov", 0}}},
                {102, "Balanced", {{"follow_rate_h", 6.5}, {"follow_rate_v", 10}, {"curve_h", 2}, {"curve_v", 0}, {"catchup_distance", 150},
                                   {"min_rate_scale", 0.35}, {"max_lag_h", 85}, {"max_lag_v", 50}, {"soft_leash", 1},
                                   {"aiming_follow", 30}, {"combat_follow", 100}, {"traversal_follow", 100},
                                   {"rotation_smoothing", 0}, {"rotation_rate", 20}, {"combat_rotation", 100}, {"traversal_rotation", 100},
                                   {"pitch_min", -60}, {"pitch_max", 40},
                                   {"exploration_distance", 100}, {"exploration_height", 0}, {"exploration_shoulder", 0}, {"exploration_fov", 0},
                                   {"sprint_distance", 100}, {"sprint_height", 0}, {"sprint_shoulder", 0}, {"sprint_fov", 0},
                                   {"combat_distance", 100}, {"combat_height", 0}, {"combat_shoulder", 0}, {"combat_fov", 0},
                                   {"focus_distance", 100}, {"focus_height", 0}, {"focus_shoulder", 0}, {"focus_fov", 0},
                                   {"aiming_distance", 100}, {"aiming_height", 0}, {"aiming_shoulder", 0}, {"aiming_fov", 0},
                                   {"traversal_distance", 100}, {"traversal_height", 0}, {"traversal_fov", 0}}},
                {103, "Cinematic", {{"follow_rate_h", 4}, {"follow_rate_v", 6}, {"curve_h", 3}, {"curve_v", 2}, {"catchup_distance", 200},
                                    {"min_rate_scale", 0.35}, {"max_lag_h", 120}, {"max_lag_v", 80}, {"soft_leash", 1},
                                    {"aiming_follow", 30}, {"combat_follow", 100}, {"traversal_follow", 100},
                                    {"rotation_smoothing", 1}, {"rotation_rate", 25}, {"combat_rotation", 100}, {"traversal_rotation", 100},
                                    {"pitch_min", -70}, {"pitch_max", 55},
                                    {"exploration_distance", 115}, {"exploration_height", 10}, {"exploration_shoulder", 10}, {"exploration_fov", 5},
                                    {"sprint_distance", 110}, {"sprint_height", 10}, {"sprint_shoulder", 10}, {"sprint_fov", 8},
                                    {"combat_distance", 110}, {"combat_height", 0}, {"combat_shoulder", 0}, {"combat_fov", 0},
                                    {"focus_distance", 110}, {"focus_height", 0}, {"focus_shoulder", 0}, {"focus_fov", 0},
                                    {"aiming_distance", 100}, {"aiming_height", 0}, {"aiming_shoulder", 0}, {"aiming_fov", 0},
                                    {"traversal_distance", 115}, {"traversal_height", 0}, {"traversal_fov", 5}}},
        };
        return presets;
    }

    auto clean_label(const std::string& raw) -> std::string
    {
        std::string s;
        for (unsigned char c : raw)
        {
            if (c >= 0x20 && c != 0x7F && c != '|') s += static_cast<char>(c);
        }
        s = trim(s);
        if (s.size() > MAX_LABEL_BYTES)
        {
            size_t cut = MAX_LABEL_BYTES;
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
            s = trim(s.substr(0, cut));
        }
        return wide_of_utf8(s) ? s : std::string{};
    }

    auto display_name(const std::string& name, const std::string& stem, int id) -> std::string
    {
        auto label = clean_label(name);
        if (label.empty()) label = clean_label(stem);
        if (label.empty()) label = "Preset " + std::to_string(id);
        return label;
    }

    auto fill_focus(Values& values) -> void
    {
        for (const char* part : {"distance", "height", "shoulder", "fov"})
        {
            std::string focus = std::string("focus_") + part, combat = std::string("combat_") + part;
            auto has = [&](const std::string& k) { return std::find_if(values.begin(), values.end(), [&](auto& e) { return e.first == k; }); };
            if (has(focus) != values.end()) continue;
            if (auto c = has(combat); c != values.end()) values.emplace_back(focus, c->second);
        }
    }

    auto parse_preset_file(std::string content) -> std::pair<std::string, Values>
    {
        if (content.starts_with("\xEF\xBB\xBF")) content.erase(0, 3);
        std::string name;
        Values values;
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = trim(line.substr(0, eq));
            auto value = trim(line.substr(eq + 1));
            if (key == "name")
            {
                name = value;
                continue;
            }
            if (!is_preset_key(key)) continue;
            try
            {
                double v = std::stod(value);
                if (!std::isfinite(v)) continue;
                auto known = std::find_if(values.begin(), values.end(), [&](auto& entry) { return entry.first == key; });
                if (known != values.end()) known->second = v;
                else values.emplace_back(key, v);
            }
            catch (...)
            {
            }
        }
        fill_focus(values);
        return {name, values};
    }

    auto normalize_preset(const Values& values) -> Values
    {
        Settings s;
        apply_values(s, values);
        Values out;
        for (auto& [key, value] : values) out.emplace_back(key, std::stod(format_number(number_of(s, key))));
        return out;
    }

    auto slot_file_content(int slot, const std::string& name, const Values& values) -> std::string
    {
        auto n = std::to_string(slot);
        std::string out = "; DWSmoothwalker Slot " + n + ", written by the mod when you save to it from the Mod Menu.\n"
                          "; Change name below to rename the slot: the menu and the banner show it after the next game start.\n"
                          "; To make a drop-in preset from it, copy this file and give the copy any other file name.\n";
        out += "name = " + name + "\n";
        for (auto& [key, value] : values) out += key + " = " + format_number(value) + "\n";
        return out;
    }

    auto list_preset_files(const std::wstring& dir) -> PresetFiles
    {
        PresetFiles out;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((dir + L"\\*.ini").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return out;
        do
        {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = data.cFileName;
            // "*.ini" also matches longer extensions through 8.3 short names.
            if (name.size() < 5 || CompareStringOrdinal(name.c_str() + name.size() - 4, 4, L".ini", 4, TRUE) != CSTR_EQUAL) continue;
            // The Mod Menu's manifest scan is recursive: a mod_settings.ini here would be read as a page too.
            if (CompareStringOrdinal(name.c_str(), -1, L"mod_settings.ini", -1, TRUE) == CSTR_EQUAL) continue;
            int slot = 0;
            for (int n = 1; n <= MAX_SLOTS && !slot; ++n)
            {
                auto expected = L"Slot " + std::to_wstring(n) + L".ini";
                if (CompareStringOrdinal(name.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL) slot = n;
            }
            if (slot) out.slots[slot] = name;
            else out.dropins.push_back(name);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        std::sort(out.dropins.begin(), out.dropins.end(),
                  [](const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
        return out;
    }

    auto read_small_file(const std::wstring& path, size_t limit) -> std::optional<std::string>
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return std::nullopt;
        auto size = static_cast<std::streamoff>(file.tellg());
        if (size < 0 || static_cast<uint64_t>(size) > limit) return std::nullopt;
        file.seekg(0);
        std::string out(static_cast<size_t>(size), '\0');
        if (size > 0 && !file.read(out.data(), size)) return std::nullopt;
        return out;
    }

    auto with_preset_choices(const std::string& manifest, const std::string& section, const std::string& values,
                             const std::string& labels) -> std::optional<std::string>
    {
        std::string out;
        out.reserve(manifest.size() + values.size() + labels.size());
        bool in_section = false, found_values = false, found_labels = false;
        size_t pos = 0;
        while (pos < manifest.size())
        {
            auto end = manifest.find('\n', pos);
            auto stop = end == std::string::npos ? manifest.size() : end;
            std::string line = manifest.substr(pos, stop - pos);
            bool cr = !line.empty() && line.back() == '\r';
            if (cr) line.pop_back();
            auto t = trim(line);
            if (!t.empty() && t.front() == '[' && t.back() == ']')
            {
                in_section = t == section;
            }
            else if (in_section && !t.empty() && t[0] != ';' && t[0] != '#')
            {
                if (auto eq = line.find('='); eq != std::string::npos)
                {
                    auto key = trim(line.substr(0, eq));
                    auto start = line.find_first_not_of(" \t", eq + 1);
                    if (start == std::string::npos) start = line.size();
                    if (key == "PresetValues")
                    {
                        line = line.substr(0, start) + values;
                        found_values = true;
                    }
                    else if (key == "PresetLabels")
                    {
                        line = line.substr(0, start) + labels;
                        found_labels = true;
                    }
                }
            }
            out += line;
            if (cr) out += '\r';
            if (end != std::string::npos) out += '\n';
            pos = stop + 1;
        }
        if (!found_values || !found_labels) return std::nullopt;
        return out;
    }
} // namespace dw::smoothwalker::settings
