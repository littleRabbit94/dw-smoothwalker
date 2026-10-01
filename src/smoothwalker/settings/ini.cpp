// smoothwalker.ini as text (settings/ini.hpp).
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "ini.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dw::smoothwalker::settings
{
    auto trim(std::string v) -> std::string
    {
        // ASCII whitespace only: std::isspace depends on the C locale, and UTF-8 bytes must survive.
        auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f'; };
        v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
        v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
        return v;
    }

    auto parse_settings(const std::string& content) -> Settings
    {
        Settings s;
        bool focus_seen = false;
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = trim(line.substr(0, eq));
            auto value = trim(line.substr(eq + 1));
            // A blank key name is kept, so it unbinds the key instead of falling back to the default.
            bool key_name = key == "toggle_key" || key == "preset_key" || key == "shoulder_key" || key == "debug_key";
            if (!key.empty() && (!value.empty() || key_name)) set_value(s, key, value);
            bool focus_position = key == "focus_distance" || key == "focus_height" || key == "focus_shoulder" || key == "focus_fov";
            if (focus_position && !value.empty()) focus_seen = true;
        }
        // The focus group's position is 0.9.0; a file without any of its four keys gets the combat values, which is what
        // focus used. focus_follow and focus_rotation say nothing about the position, so they do not count.
        if (!focus_seen)
        {
            s.focus_distance = s.combat_distance;
            s.focus_height = s.combat_height;
            s.focus_shoulder = s.combat_shoulder;
            s.focus_fov = s.combat_fov;
        }
        sanitize(s);
        return s;
    }

    auto parse_numbers(const std::string& content) -> std::map<std::string, double>
    {
        std::map<std::string, double> out;
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = trim(line.substr(0, eq));
            if (!is_numeric_key(key)) continue;
            try
            {
                double v = std::stod(trim(line.substr(eq + 1)));
                if (std::isfinite(v)) out[key] = v;
            }
            catch (...)
            {
            }
        }
        return out;
    }

    auto format_number(double v) -> std::string
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", v);
        return buffer;
    }

    auto rewrite_numbers(const std::string& content, const Values& values) -> std::string
    {
        std::string out;
        std::istringstream in(content);
        std::string line;
        bool first = true;
        while (std::getline(in, line))
        {
            bool cr = !line.empty() && line.back() == '\r';
            if (cr) line.pop_back();
            auto eq = line.find('=');
            auto comment = line.find_first_of(";#");
            auto trimmed = trim(line);
            if (eq != std::string::npos && (comment == std::string::npos || comment > eq) && !trimmed.empty() && trimmed[0] != ';' &&
                trimmed[0] != '#')
            {
                auto key = trim(line.substr(0, eq));
                for (auto& [name, value] : values)
                {
                    if (name != key) continue;
                    auto value_end = comment == std::string::npos ? line.size() : comment;
                    auto start = line.find_first_not_of(" \t", eq + 1);
                    if (start == std::string::npos || start >= value_end) break;
                    auto stop = line.find_last_not_of(" \t", value_end - 1) + 1;
                    line = line.substr(0, start) + format_number(value) + line.substr(stop);
                    break;
                }
            }
            if (!first) out += '\n';
            first = false;
            out += line;
            if (cr) out += '\r';
        }
        if (!content.empty() && content.back() == '\n') out += '\n';
        return out;
    }

    auto shipped_comment(const std::string& key) -> const char*
    {
        static const std::map<std::string, const char*> comments{
            {"enabled", "0 is the game's own camera: no follow, no camera position changes, preset and shoulder keys ignored"},
            {"follow_rate_h", "horizontal catch-up rate"},
            {"follow_rate_v", "vertical catch-up rate"},
            {"curve_h", "0 constant, 1 linear, 2 smoothstep, 3 ease in-out"},
            {"catchup_distance", "cm of lag at which a curve reaches the full rate"},
            {"min_rate_scale", "rate multiplier at zero lag for curves 1-3"},
            {"max_lag_h", "cm the camera may fall behind horizontally; 0 is no trail, and that axis's rate and curve do nothing"},
            {"max_lag_v", "cm vertically; 0 as above"},
            {"soft_leash", "1 eases into the limit, 0 stops hard at it"},
            {"aiming_follow", "percent of the trail and the turning smoothing kept while you aim: 0 none, 100 all"},
            {"combat_follow", "percent of the trail kept while a combat camera is up: 0 none, 100 all"},
            {"traversal_follow", "percent of the trail kept while a traversal camera is up: 0 none, 100 all"},
            {"focus_follow", "percent of the trail kept while a focus camera is up: 0 none, 100 all"},
            {"interior_follow", "percent of the trail kept indoors (the game's interior camera type): 0 none, 100 all"},
            {"combat_rotation", "percent of the turning smoothing kept while a combat camera is up: 0 none, 100 all"},
            {"traversal_rotation", "percent of the turning smoothing kept while a traversal camera is up: 0 none, 100 all"},
            {"focus_rotation", "percent of the turning smoothing kept while a focus camera is up: 0 none, 100 all"},
            {"interior_rotation", "percent of the turning smoothing kept indoors (the game's interior camera type): 0 none, 100 all"},
            {"wall_clamp", "keep the camera in front of walls the game pulled it in for"},
            {"reset_distance", "cm moved in one frame that counts as a teleport: snap instead of smoothing"},
            {"reset_gap", "s without camera updates (cutscene, free camera, load) before a snap"},
            {"show_banner", "show the preset name and on/off in the game's region banner"},
            {"camera_tuning", "0 leaves each camera mode's position, field of view and look limits as the game (or another camera mod) set them"},
            {"exploration_distance", "percent of the game's distance behind the character"},
            {"exploration_height", "cm higher"},
            {"exploration_shoulder", "cm further out to the side (centered camera modes are left alone)"},
            {"exploration_fov", "degrees of field of view added"},
            {"sprint_distance", "percent of the game's distance behind the character"},
            {"sprint_height", "cm higher"},
            {"sprint_shoulder", "cm further out to the side (centered camera modes are left alone)"},
            {"sprint_fov", "degrees of field of view added"},
            {"speed_blend", "percent of the sprinting settings above that arrive with speed, from a walk to a sprint; 0 switches at the sprint as the game does"},
            {"speed_blend_start", "cm/s at which that blend starts (walk is about 131)"},
            {"speed_blend_full", "cm/s at which it is complete (sprint is about 558)"},
            {"speed_blend_rise", "s the speed takes to ease up, per stage of two (higher: a gentler start)"},
            {"speed_blend_fall", "s the speed takes to ease down, per stage of two (higher: turns pump the camera less, stops settle slower)"},
            {"combat_distance", "percent of the game's distance behind the character"},
            {"combat_height", "cm higher"},
            {"combat_shoulder", "cm further out to the side (centered camera modes are left alone)"},
            {"combat_fov", "degrees of field of view added"},
            {"focus_distance", "percent of the game's distance behind the character"},
            {"focus_height", "cm higher"},
            {"focus_shoulder", "cm further out to the side (centered camera modes are left alone)"},
            {"focus_fov", "degrees of field of view added"},
            {"aiming_distance", "percent of the game's distance behind the character"},
            {"aiming_height", "cm higher"},
            {"aiming_shoulder", "cm further out to the side (centered camera modes are left alone)"},
            {"aiming_fov", "degrees of field of view added"},
            {"traversal_distance", "percent of the game's distance behind the character"},
            {"traversal_height", "cm higher"},
            {"traversal_fov", "degrees of field of view added"},
            {"interior_distance", "percent of the distance indoors, on top of each group's"},
            {"interior_height", "cm higher indoors"},
            {"interior_shoulder", "cm further out to the side indoors (centered camera modes are left alone)"},
            {"interior_fov", "degrees of field of view added indoors"},
            {"shoulder_swap", "1 puts the camera over the other shoulder"},
            {"pitch_min", "how far down you can look (the game: -60)"},
            {"pitch_max", "how far up you can look (the game: 40)"},
            {"position_transition", "s a position change or shoulder swap glides over; 0 snaps"},
            {"preset", "0 Custom, 101 Tight, 102 Balanced, 103 Cinematic, 1-6 a slot, 201+ a preset from the config/presets folder: shows the matching preset; change it to load one, or to an unused slot to save your settings into it"},
            {"log_stats", "every 5 s in UE4SS.log: smoothed frames, mean lag, wall clamp share, microseconds per frame"},
            {"log_verbose", "1 adds detail to UE4SS.log: player discovery, offsets, camera mode layouts, focus, combat and traversal camera changes and indoor switches"},
            {"debug_overlay", "1 shows a live panel of the camera state at the top right of the screen"},
            {"debug_markers", "1 draws the camera's trail around your character"},
            {"debug_key", "shows and hides the panel and the markers in game (blank: no key)"},
        };
        auto found = comments.find(key);
        return found == comments.end() ? nullptr : found->second;
    }

    auto with_missing_keys(const std::string& content, const Settings& s) -> std::pair<std::string, std::vector<std::string>>
    {
        struct Line
        {
            size_t start, text_end, end; // text_end: before "\r\n" or "\n"; end: after it
        };
        std::vector<Line> lines;
        for (size_t pos = 0; pos < content.size();)
        {
            auto nl = content.find('\n', pos);
            if (nl == std::string::npos)
            {
                lines.push_back({pos, content.size(), content.size()});
                break;
            }
            lines.push_back({pos, nl > pos && content[nl - 1] == '\r' ? nl - 1 : nl, nl + 1});
            pos = nl + 1;
        }
        auto first_nl = content.find('\n');
        std::string newline = first_nl != std::string::npos && first_nl > 0 && content[first_nl - 1] == '\r' ? "\r\n" : "\n";

        std::map<std::string, size_t> line_of; // a repeated key: its last line, as parse_numbers keeps the last value
        for (size_t i = 0; i < lines.size(); ++i)
        {
            auto line = content.substr(lines[i].start, lines[i].text_end - lines[i].start);
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = trim(line.substr(0, eq));
            if (is_numeric_key(key) || key == "debug_key") line_of[key] = i;
        }

        std::vector<const char*> order; // NUMERIC_KEYS, with debug_key after debug_markers
        for (auto* key : NUMERIC_KEYS)
        {
            order.push_back(key);
            if (std::string_view(key) == "debug_markers") order.push_back("debug_key");
        }

        std::vector<std::string> added;
        std::map<size_t, std::string> after; // line index -> the lines added after it, each led by a newline
        std::string tail;                    // an empty file: the added lines alone
        std::optional<size_t> anchor;        // the line of the last key seen in `order`; an added key goes after the one before it
        for (auto* key : order)
        {
            if (auto found = line_of.find(key); found != line_of.end())
            {
                anchor = found->second;
                continue;
            }
            // Laid out as the shipped file: the key padded to 16, the value to 6, then its comment.
            auto name = std::string(key), value = name == "debug_key" ? s.debug_key : format_number(number_of(s, key));
            auto text = name + std::string(name.size() < 16 ? 16 - name.size() : 0, ' ') + " = " + value;
            if (auto* comment = shipped_comment(name)) text += std::string(value.size() < 6 ? 6 - value.size() : 1, ' ') + "; " + comment;
            if (lines.empty()) tail += text + newline;
            else after[anchor.value_or(lines.size() - 1)] += newline + text;
            added.push_back(key);
        }
        if (added.empty()) return {content, added};

        std::string out;
        out.reserve(content.size() + tail.size() + 64 * added.size());
        for (size_t i = 0; i < lines.size(); ++i)
        {
            out.append(content, lines[i].start, lines[i].text_end - lines[i].start);
            if (auto extra = after.find(i); extra != after.end()) out += extra->second;
            out.append(content, lines[i].text_end, lines[i].end - lines[i].text_end);
        }
        out += tail;
        return {out, added};
    }

    auto read_file(const std::string& path) -> std::optional<std::string>
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    auto write_file(const std::string& path, const std::string& content) -> bool
    {
        auto tmp = path + ".dwsc.tmp";
        bool written = false;
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (file)
            {
                file << content;
                file.flush();
                written = static_cast<bool>(file);
            }
        }
        if (written && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileA(tmp.c_str());
        return false;
    }
} // namespace dw::smoothwalker::settings
