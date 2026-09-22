// Settings (smoothwalker.ini, rewritten in place by the Dawnwalker Mod Menu and re-read on change, written back by
// the mod while the camera is live) and presets (config/presets/*.ini: slot files written by the mod,
// drop-ins added by hand). Key names are read at startup: the menu can only move numbers.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dwsc
{
    // Saved preset slots, ids 1..MAX_SLOTS (60 ceiling: see MAX_DROPINS); mod_settings.ini lists them by hand.
    inline constexpr int MAX_SLOTS = 6;

    struct Settings
    {
        bool enabled = true;
        std::string toggle_key;           // empty: not bound
        std::string preset_key;
        std::string shoulder_key = "V";

        double follow_rate_h = 6.5;       // 1/s
        double follow_rate_v = 10.0;      // 1/s
        int curve_h = 2;                  // 0 constant, 1 linear, 2 smoothstep, 3 ease in-out
        int curve_v = 0;
        double catchup_distance = 150.0;  // cm of lag at which a curve reaches full rate
        double min_rate_scale = 0.35;     // rate multiplier at zero lag, curves 1-3
        double max_lag_h = 85.0;          // cm
        double max_lag_v = 50.0;
        bool soft_leash = true;
        double aiming_follow = 30.0;      // percent of the trail and the turning smoothing kept while aiming

        bool rotation_smoothing = false;
        double rotation_rate = 20.0;

        bool wall_clamp = true;
        double reset_distance = 500.0;    // cm moved in one frame that counts as a teleport
        double reset_gap = 0.25;          // s without a view update before the camera snaps

        bool show_banner = true;

        // Camera position (mode_tuning.hpp). These defaults leave the game's modes as shipped.
        bool camera_tuning = true;
        double exploration_distance = 100, exploration_height = 0, exploration_shoulder = 0, exploration_fov = 0;
        double sprint_distance = 100, sprint_height = 0, sprint_shoulder = 0, sprint_fov = 0;
        double combat_distance = 100, combat_height = 0, combat_shoulder = 0, combat_fov = 0;
        double focus_distance = 100, focus_height = 0, focus_shoulder = 0, focus_fov = 0; // 0.9.0; a file without them takes combat's
        double aiming_distance = 100, aiming_height = 0, aiming_shoulder = 0, aiming_fov = 0;
        double traversal_distance = 100, traversal_height = 0, traversal_fov = 0;
        bool shoulder_swap = false;
        double pitch_min = -60, pitch_max = 40;
        double position_transition = 0.5; // s; 0 snaps

        int preset = 102;                 // the preset the live settings match: 0 Custom, 101-103 built-in, 1-MAX_SLOTS slot, FIRST_DROPIN_ID on drop-in

        bool log_stats = false;
        bool log_trace = false; // per-frame vertical-follow trace, written to the log around every crouch and stand
    };

    // A preset is a camera look: follow, turning, the look limits and camera position. Not the switches (enabled,
    // camera_tuning, shoulder_swap, show_banner, log_stats, log_trace), aiming_follow, the safety values (wall_clamp,
    // reset_distance, reset_gap), position_transition, the key names, or preset.
    inline const std::array<const char*, 36> PRESET_KEYS{
            "follow_rate_h", "follow_rate_v", "curve_h", "curve_v", "catchup_distance", "min_rate_scale", "max_lag_h", "max_lag_v",
            "soft_leash", "rotation_smoothing", "rotation_rate",
            "pitch_min", "pitch_max", "exploration_distance", "exploration_height", "exploration_shoulder",
            "exploration_fov", "sprint_distance", "sprint_height", "sprint_shoulder", "sprint_fov", "combat_distance", "combat_height",
            "combat_shoulder", "combat_fov", "focus_distance", "focus_height", "focus_shoulder", "focus_fov", "aiming_distance", "aiming_height", "aiming_shoulder", "aiming_fov", "traversal_distance",
            "traversal_height", "traversal_fov"};

    // Every numeric setting: the ones the Mod Menu can move and the mod writes back.
    inline const std::array<const char*, 48> NUMERIC_KEYS{
            "enabled", "follow_rate_h", "follow_rate_v", "curve_h", "curve_v", "catchup_distance", "min_rate_scale", "max_lag_h",
            "max_lag_v", "soft_leash", "aiming_follow", "rotation_smoothing", "rotation_rate", "wall_clamp", "reset_distance", "reset_gap", "show_banner",
            "camera_tuning", "exploration_distance", "exploration_height", "exploration_shoulder", "exploration_fov", "sprint_distance",
            "sprint_height", "sprint_shoulder", "sprint_fov", "combat_distance", "combat_height", "combat_shoulder", "combat_fov", "focus_distance", "focus_height", "focus_shoulder", "focus_fov",
            "aiming_distance", "aiming_height", "aiming_shoulder", "aiming_fov", "traversal_distance", "traversal_height", "traversal_fov",
            "shoulder_swap", "pitch_min", "pitch_max", "position_transition", "preset", "log_stats", "log_trace"};

    inline auto is_preset_key(const std::string& key) -> bool
    {
        return std::find_if(PRESET_KEYS.begin(), PRESET_KEYS.end(), [&](const char* k) { return key == k; }) != PRESET_KEYS.end();
    }

    inline auto is_numeric_key(const std::string& key) -> bool
    {
        return std::find_if(NUMERIC_KEYS.begin(), NUMERIC_KEYS.end(), [&](const char* k) { return key == k; }) != NUMERIC_KEYS.end();
    }

    using Values = std::vector<std::pair<std::string, double>>;

    inline auto trim(std::string v) -> std::string
    {
        // ASCII whitespace only: std::isspace depends on the C locale, and UTF-8 bytes must survive.
        auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f'; };
        v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
        v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
        return v;
    }

    inline auto set_value(Settings& s, const std::string& key, const std::string& value) -> void
    {
        // std::stod accepts "nan" and "inf", which clamps pass through.
        auto number = [&](double& out) {
            try
            {
                double v = std::stod(value);
                if (std::isfinite(v)) out = v;
            }
            catch (...)
            {
            }
        };
        auto integer = [&](int& out) {
            try
            {
                double v = std::stod(value);
                if (std::isfinite(v) && std::abs(v) < 1e6) out = static_cast<int>(std::lround(v));
            }
            catch (...)
            {
            }
        };
        auto flag = [&](bool& out) {
            try { out = std::stod(value) != 0.0; } catch (...) { out = value == "true" || value == "True"; }
        };

        if (key == "enabled") flag(s.enabled);
        else if (key == "toggle_key") s.toggle_key = value;
        else if (key == "preset_key") s.preset_key = value;
        else if (key == "follow_rate_h") number(s.follow_rate_h);
        else if (key == "follow_rate_v") number(s.follow_rate_v);
        else if (key == "curve_h") integer(s.curve_h);
        else if (key == "curve_v") integer(s.curve_v);
        else if (key == "catchup_distance") number(s.catchup_distance);
        else if (key == "min_rate_scale") number(s.min_rate_scale);
        else if (key == "max_lag_h") number(s.max_lag_h);
        else if (key == "max_lag_v") number(s.max_lag_v);
        else if (key == "soft_leash") flag(s.soft_leash);
        else if (key == "aiming_follow") number(s.aiming_follow);
        else if (key == "rotation_smoothing") flag(s.rotation_smoothing);
        else if (key == "rotation_rate") number(s.rotation_rate);
        else if (key == "wall_clamp") flag(s.wall_clamp);
        else if (key == "reset_distance") number(s.reset_distance);
        else if (key == "reset_gap") number(s.reset_gap);
        else if (key == "show_banner") flag(s.show_banner);
        else if (key == "shoulder_key") s.shoulder_key = value;
        else if (key == "camera_tuning") flag(s.camera_tuning);
        else if (key == "shoulder_swap") flag(s.shoulder_swap);
        else if (key == "position_transition") number(s.position_transition);
        else if (key == "exploration_distance") number(s.exploration_distance);
        else if (key == "exploration_height") number(s.exploration_height);
        else if (key == "exploration_shoulder") number(s.exploration_shoulder);
        else if (key == "exploration_fov") number(s.exploration_fov);
        else if (key == "sprint_distance") number(s.sprint_distance);
        else if (key == "sprint_height") number(s.sprint_height);
        else if (key == "sprint_shoulder") number(s.sprint_shoulder);
        else if (key == "sprint_fov") number(s.sprint_fov);
        else if (key == "combat_distance") number(s.combat_distance);
        else if (key == "combat_height") number(s.combat_height);
        else if (key == "combat_shoulder") number(s.combat_shoulder);
        else if (key == "combat_fov") number(s.combat_fov);
        else if (key == "focus_distance") number(s.focus_distance);
        else if (key == "focus_height") number(s.focus_height);
        else if (key == "focus_shoulder") number(s.focus_shoulder);
        else if (key == "focus_fov") number(s.focus_fov);
        else if (key == "aiming_distance") number(s.aiming_distance);
        else if (key == "aiming_height") number(s.aiming_height);
        else if (key == "aiming_shoulder") number(s.aiming_shoulder);
        else if (key == "aiming_fov") number(s.aiming_fov);
        else if (key == "traversal_distance") number(s.traversal_distance);
        else if (key == "traversal_height") number(s.traversal_height);
        else if (key == "traversal_fov") number(s.traversal_fov);
        else if (key == "pitch_min") number(s.pitch_min);
        else if (key == "pitch_max") number(s.pitch_max);
        else if (key == "preset") integer(s.preset);
        else if (key == "log_stats") flag(s.log_stats);
        else if (key == "log_trace") flag(s.log_trace);
    }

    // The Mod Menu's ranges (mod_settings.ini). Live values are written back into the file, and a value outside
    // its range stops the whole page from opening.
    inline auto sanitize(Settings& s) -> void
    {
        s.curve_h = std::clamp(s.curve_h, 0, 3);
        s.curve_v = std::clamp(s.curve_v, 0, 3);
        s.follow_rate_h = std::clamp(s.follow_rate_h, 0.5, 30.0);
        s.follow_rate_v = std::clamp(s.follow_rate_v, 0.5, 30.0);
        s.catchup_distance = std::clamp(s.catchup_distance, 10.0, 500.0);
        s.min_rate_scale = std::clamp(s.min_rate_scale, 0.05, 1.0);
        s.max_lag_h = std::clamp(s.max_lag_h, 0.0, 300.0);
        s.max_lag_v = std::clamp(s.max_lag_v, 0.0, 200.0);
        s.aiming_follow = std::clamp(s.aiming_follow, 0.0, 100.0);
        s.rotation_rate = std::clamp(s.rotation_rate, 1.0, 60.0);
        s.reset_distance = std::clamp(s.reset_distance, 100.0, 3000.0);
        s.reset_gap = std::clamp(s.reset_gap, 0.05, 2.0);
        for (double* d : {&s.exploration_distance, &s.sprint_distance, &s.combat_distance, &s.focus_distance, &s.aiming_distance, &s.traversal_distance})
        {
            *d = std::clamp(*d, 25.0, 250.0); // below about 50 the camera is inside the game's 100 cm clipping radius on most modes
        }
        for (double* h : {&s.exploration_height, &s.sprint_height, &s.combat_height, &s.focus_height, &s.aiming_height, &s.traversal_height})
        {
            *h = std::clamp(*h, -50.0, 100.0);
        }
        for (double* o : {&s.exploration_shoulder, &s.sprint_shoulder, &s.combat_shoulder, &s.focus_shoulder, &s.aiming_shoulder})
        {
            *o = std::clamp(*o, -60.0, 100.0);
        }
        for (double* f : {&s.exploration_fov, &s.sprint_fov, &s.combat_fov, &s.focus_fov, &s.aiming_fov, &s.traversal_fov})
        {
            *f = std::clamp(*f, -30.0, 30.0);
        }
        s.pitch_min = std::clamp(s.pitch_min, -89.0, -30.0);
        s.pitch_max = std::clamp(s.pitch_max, 20.0, 89.0);
        s.position_transition = std::clamp(s.position_transition, 0.0, 2.0);
    }

    inline auto parse_settings(const std::string& content) -> Settings
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
            bool key_name = key == "toggle_key" || key == "preset_key" || key == "shoulder_key";
            if (!key.empty() && (!value.empty() || key_name)) set_value(s, key, value);
            if (key.rfind("focus_", 0) == 0 && !value.empty()) focus_seen = true;
        }
        // The focus group is 0.9.0; a file from before it gets its combat values, which is what focus used.
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

    // The file's numeric keys as written, unsanitized: the baseline a later change is diffed against. Non-finite
    // values are left out; a repeated key keeps its last value, matching parse_settings.
    inline auto parse_numbers(const std::string& content) -> std::map<std::string, double>
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

    inline auto number_of(const Settings& s, const std::string& key) -> double
    {
        auto flag = [](bool b) { return b ? 1.0 : 0.0; };
        if (key == "enabled") return flag(s.enabled);
        if (key == "follow_rate_h") return s.follow_rate_h;
        if (key == "follow_rate_v") return s.follow_rate_v;
        if (key == "curve_h") return s.curve_h;
        if (key == "curve_v") return s.curve_v;
        if (key == "catchup_distance") return s.catchup_distance;
        if (key == "min_rate_scale") return s.min_rate_scale;
        if (key == "max_lag_h") return s.max_lag_h;
        if (key == "max_lag_v") return s.max_lag_v;
        if (key == "soft_leash") return flag(s.soft_leash);
        if (key == "aiming_follow") return s.aiming_follow;
        if (key == "rotation_smoothing") return flag(s.rotation_smoothing);
        if (key == "rotation_rate") return s.rotation_rate;
        if (key == "wall_clamp") return flag(s.wall_clamp);
        if (key == "reset_distance") return s.reset_distance;
        if (key == "reset_gap") return s.reset_gap;
        if (key == "show_banner") return flag(s.show_banner);
        if (key == "camera_tuning") return flag(s.camera_tuning);
        if (key == "exploration_distance") return s.exploration_distance;
        if (key == "exploration_height") return s.exploration_height;
        if (key == "exploration_shoulder") return s.exploration_shoulder;
        if (key == "exploration_fov") return s.exploration_fov;
        if (key == "sprint_distance") return s.sprint_distance;
        if (key == "sprint_height") return s.sprint_height;
        if (key == "sprint_shoulder") return s.sprint_shoulder;
        if (key == "sprint_fov") return s.sprint_fov;
        if (key == "combat_distance") return s.combat_distance;
        if (key == "combat_height") return s.combat_height;
        if (key == "combat_shoulder") return s.combat_shoulder;
        if (key == "combat_fov") return s.combat_fov;
        if (key == "focus_distance") return s.focus_distance;
        if (key == "focus_height") return s.focus_height;
        if (key == "focus_shoulder") return s.focus_shoulder;
        if (key == "focus_fov") return s.focus_fov;
        if (key == "aiming_distance") return s.aiming_distance;
        if (key == "aiming_height") return s.aiming_height;
        if (key == "aiming_shoulder") return s.aiming_shoulder;
        if (key == "aiming_fov") return s.aiming_fov;
        if (key == "traversal_distance") return s.traversal_distance;
        if (key == "traversal_height") return s.traversal_height;
        if (key == "traversal_fov") return s.traversal_fov;
        if (key == "shoulder_swap") return flag(s.shoulder_swap);
        if (key == "pitch_min") return s.pitch_min;
        if (key == "pitch_max") return s.pitch_max;
        if (key == "position_transition") return s.position_transition;
        if (key == "preset") return s.preset;
        if (key == "log_stats") return flag(s.log_stats);
        if (key == "log_trace") return flag(s.log_trace);
        return 0.0;
    }

    inline auto preset_of(const Settings& s) -> Values
    {
        Values out;
        for (auto* key : PRESET_KEYS) out.emplace_back(key, number_of(s, key));
        return out;
    }

    inline auto format_number(double v) -> std::string
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", v);
        return buffer;
    }

    // Full precision, so a value applied from the file compares equal to it.
    inline auto apply_values(Settings& s, const Values& values) -> void
    {
        char buffer[64];
        for (auto& [key, value] : values)
        {
            std::snprintf(buffer, sizeof(buffer), "%.17g", value);
            set_value(s, key, buffer);
        }
        sanitize(s);
    }

    // In place, keeping layout and comments, as the Mod Menu does.
    inline auto rewrite_numbers(const std::string& content, const Values& values) -> std::string
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

    inline auto read_file(const std::string& path) -> std::optional<std::string>
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Temp file plus rename, so the Mod Menu never reads a half-written file. A failed write removes the temp file.
    inline auto write_file(const std::string& path, const std::string& content) -> bool
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

    struct NamedPreset
    {
        int id;
        const char* name;
        Values values;
    };

    // Menu ids 101-103, in cycle order, each with all PRESET_KEYS. Balanced matches the shipped defaults on
    // every key. Every value must sit inside its Mod Menu range and on its slider step.
    inline auto builtin_presets() -> const std::vector<NamedPreset>&
    {
        static const std::vector<NamedPreset> presets{
                {101, "Tight", {{"follow_rate_h", 12}, {"follow_rate_v", 20}, {"curve_h", 0}, {"curve_v", 0}, {"catchup_distance", 100},
                                {"min_rate_scale", 0.5}, {"max_lag_h", 40}, {"max_lag_v", 20}, {"soft_leash", 1},
                                {"rotation_smoothing", 0}, {"rotation_rate", 20},
                                {"pitch_min", -60}, {"pitch_max", 40},
                                {"exploration_distance", 90}, {"exploration_height", 0}, {"exploration_shoulder", 0}, {"exploration_fov", 0},
                                {"sprint_distance", 90}, {"sprint_height", 0}, {"sprint_shoulder", 0}, {"sprint_fov", 0},
                                {"combat_distance", 95}, {"combat_height", 0}, {"combat_shoulder", 0}, {"combat_fov", 0},
                                {"focus_distance", 95}, {"focus_height", 0}, {"focus_shoulder", 0}, {"focus_fov", 0},
                                {"aiming_distance", 100}, {"aiming_height", 0}, {"aiming_shoulder", 0}, {"aiming_fov", 0},
                                {"traversal_distance", 95}, {"traversal_height", 0}, {"traversal_fov", 0}}},
                {102, "Balanced", {{"follow_rate_h", 6.5}, {"follow_rate_v", 10}, {"curve_h", 2}, {"curve_v", 0}, {"catchup_distance", 150},
                                   {"min_rate_scale", 0.35}, {"max_lag_h", 85}, {"max_lag_v", 50}, {"soft_leash", 1},
                                   {"rotation_smoothing", 0}, {"rotation_rate", 20},
                                   {"pitch_min", -60}, {"pitch_max", 40},
                                   {"exploration_distance", 100}, {"exploration_height", 0}, {"exploration_shoulder", 0}, {"exploration_fov", 0},
                                   {"sprint_distance", 100}, {"sprint_height", 0}, {"sprint_shoulder", 0}, {"sprint_fov", 0},
                                   {"combat_distance", 100}, {"combat_height", 0}, {"combat_shoulder", 0}, {"combat_fov", 0},
                                   {"focus_distance", 100}, {"focus_height", 0}, {"focus_shoulder", 0}, {"focus_fov", 0},
                                   {"aiming_distance", 100}, {"aiming_height", 0}, {"aiming_shoulder", 0}, {"aiming_fov", 0},
                                   {"traversal_distance", 100}, {"traversal_height", 0}, {"traversal_fov", 0}}},
                {103, "Cinematic", {{"follow_rate_h", 4}, {"follow_rate_v", 6}, {"curve_h", 3}, {"curve_v", 2}, {"catchup_distance", 200},
                                    {"min_rate_scale", 0.35}, {"max_lag_h", 120}, {"max_lag_v", 80}, {"soft_leash", 1},
                                    {"rotation_smoothing", 1}, {"rotation_rate", 25},
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

    // One preset of this session: a built-in (101-103), a slot (1..MAX_SLOTS) or a drop-in (201 on).
    struct Preset
    {
        int id;
        std::string name; // UTF-8
        Values values;
    };

    inline constexpr int FIRST_DROPIN_ID = 201;
    // The Mod Menu takes at most 64 values in a picker: Custom, the 3 built-ins, the slots, then drop-ins.
    inline constexpr int MAX_DROPINS = 64 - 4 - MAX_SLOTS;
    inline constexpr size_t MAX_PRESET_FILE = 64 * 1024;
    inline constexpr size_t MAX_LABEL_BYTES = 48;

    inline auto utf8_of(const std::wstring& w) -> std::optional<std::string>
    {
        if (w.empty()) return std::string{};
        int size = static_cast<int>(w.size());
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), size, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::nullopt;
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), size, out.data(), n, nullptr, nullptr);
        return out;
    }

    // Strict: nullopt for invalid UTF-8, which would make the Mod Menu skip the whole manifest.
    inline auto wide_of_utf8(const std::string& s) -> std::optional<std::wstring>
    {
        if (s.empty()) return std::wstring{};
        int size = static_cast<int>(s.size());
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), size, nullptr, 0);
        if (n <= 0) return std::nullopt;
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), size, out.data(), n);
        return out;
    }

    // For log and banner text: invalid bytes become U+FFFD.
    inline auto to_wide(const std::string& s) -> std::wstring
    {
        if (s.empty()) return {};
        int size = static_cast<int>(s.size());
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), size, nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), size, out.data(), n);
        return out;
    }

    // PresetLabels is '|'-separated and must be valid UTF-8 without control characters (else the menu skips the page).
    inline auto clean_label(const std::string& raw) -> std::string
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

    inline auto display_name(const std::string& name, const std::string& stem, int id) -> std::string
    {
        auto label = clean_label(name);
        if (label.empty()) label = clean_label(stem);
        if (label.empty()) label = "Preset " + std::to_string(id);
        return label;
    }

    // Values come back raw; normalize_preset clamps them.
    // A preset written before 0.9.0 has no focus keys: it gets its combat values, so it loads and matches as it did.
    inline auto fill_focus(Values& values) -> void
    {
        for (const char* part : {"distance", "height", "shoulder", "fov"})
        {
            std::string focus = std::string("focus_") + part, combat = std::string("combat_") + part;
            auto has = [&](const std::string& k) { return std::find_if(values.begin(), values.end(), [&](auto& e) { return e.first == k; }); };
            if (has(focus) != values.end()) continue;
            if (auto c = has(combat); c != values.end()) values.emplace_back(focus, c->second);
        }
    }

    inline auto parse_preset_file(std::string content) -> std::pair<std::string, Values>
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

    // Values as they'll be live after loading and as a flush writes them: applied through the normal path (clamps,
    // rounding, flags), then round-tripped through %.6g so a loaded preset still matches its own picker entry.
    inline auto normalize_preset(const Values& values) -> Values
    {
        Settings s;
        apply_values(s, values);
        Values out;
        for (auto& [key, value] : values) out.emplace_back(key, std::stod(format_number(number_of(s, key))));
        return out;
    }

    // name: the slot's display name, kept across saves.
    inline auto slot_file_content(int slot, const std::string& name, const Values& values) -> std::string
    {
        auto n = std::to_string(slot);
        std::string out = "; DWSmoothwalker Slot " + n + ", written by the mod when you save to it from the Mod Menu.\n"
                          "; Change name below to rename the slot: the menu and the banner show it after the next game start.\n"
                          "; To make a drop-in preset from it, copy this file and give the copy any other file name.\n";
        out += "name = " + name + "\n";
        for (auto& [key, value] : values) out += key + " = " + format_number(value) + "\n";
        return out;
    }

    struct PresetFiles
    {
        std::map<int, std::wstring> slots;  // slot number -> file name
        std::vector<std::wstring> dropins;  // file names, sorted case-insensitively
    };

    // The game path and drop-in file names may be non-ASCII.
    inline auto list_preset_files(const std::wstring& dir) -> PresetFiles
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

    inline auto read_small_file(const std::wstring& path, size_t limit) -> std::optional<std::string>
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

    // section: "[Setting.preset]". nullopt if the section or either line is missing.
    inline auto with_preset_choices(const std::string& manifest, const std::string& section, const std::string& values,
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
} // namespace dwsc
