// Settings (smoothcam.ini, rewritten in place by the Dawnwalker Mod Menu and re-read on change) and presets
// (presets.ini, written only by the mod). Key names are read at startup: the menu can only move numbers.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
    struct Settings
    {
        bool enabled = true;
        std::string toggle_key = "O";
        std::string preset_key = "V";
        std::string shoulder_key = "N";

        double follow_rate_h = 8.0;       // 1/s
        double follow_rate_v = 10.0;      // 1/s
        int curve_h = 2;                  // 0 exponential, 1 linear, 2 smoothstep, 3 ease in-out
        int curve_v = 0;
        double catchup_distance = 150.0;  // cm of lag at which a curve reaches full rate
        double min_rate_scale = 0.35;     // rate multiplier at zero lag, curves 1-3
        double max_lag_h = 70.0;          // cm
        double max_lag_v = 50.0;
        bool soft_leash = true;

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
        double aiming_distance = 100, aiming_height = 0, aiming_shoulder = 0, aiming_fov = 0;
        double traversal_distance = 100, traversal_height = 0, traversal_fov = 0;
        double game_lag_scale = 1;        // higher is tighter
        bool shoulder_swap = false;
        double pitch_min = -60, pitch_max = 40;
        double position_transition = 0.5; // s; 0 snaps

        int preset_load = 0;              // menu action, reset to 0: 101-103 built-in, 1-6 slot
        int preset_save = 0;              // menu action, reset to 0: 1-6 slot

        bool log_stats = false;
    };

    // A preset carries the feel only, not switches, keys, safety limits or camera position.
    inline const std::array<const char*, 12> PRESET_KEYS{"follow_rate_h", "follow_rate_v", "curve_h", "curve_v",
                                                          "catchup_distance", "min_rate_scale", "max_lag_h", "max_lag_v",
                                                          "soft_leash", "rotation_smoothing", "rotation_rate", "wall_clamp"};

    using Values = std::vector<std::pair<std::string, double>>;

    inline auto trim(std::string v) -> std::string
    {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
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
        else if (key == "aiming_distance") number(s.aiming_distance);
        else if (key == "aiming_height") number(s.aiming_height);
        else if (key == "aiming_shoulder") number(s.aiming_shoulder);
        else if (key == "aiming_fov") number(s.aiming_fov);
        else if (key == "traversal_distance") number(s.traversal_distance);
        else if (key == "traversal_height") number(s.traversal_height);
        else if (key == "traversal_fov") number(s.traversal_fov);
        else if (key == "game_lag_scale") number(s.game_lag_scale);
        else if (key == "pitch_min") number(s.pitch_min);
        else if (key == "pitch_max") number(s.pitch_max);
        else if (key == "preset_load") integer(s.preset_load);
        else if (key == "preset_save") integer(s.preset_save);
        else if (key == "log_stats") flag(s.log_stats);
    }

    // The Mod Menu's ranges (mod_settings.ini). Presets are written back into the file, and a value outside
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
        s.rotation_rate = std::clamp(s.rotation_rate, 1.0, 60.0);
        s.reset_distance = std::clamp(s.reset_distance, 100.0, 3000.0);
        s.reset_gap = std::clamp(s.reset_gap, 0.05, 2.0);
        for (double* d : {&s.exploration_distance, &s.sprint_distance, &s.combat_distance, &s.aiming_distance, &s.traversal_distance})
        {
            *d = std::clamp(*d, 50.0, 250.0);
        }
        for (double* h : {&s.exploration_height, &s.sprint_height, &s.combat_height, &s.aiming_height, &s.traversal_height})
        {
            *h = std::clamp(*h, -50.0, 100.0);
        }
        for (double* o : {&s.exploration_shoulder, &s.sprint_shoulder, &s.combat_shoulder, &s.aiming_shoulder})
        {
            *o = std::clamp(*o, -60.0, 100.0);
        }
        for (double* f : {&s.exploration_fov, &s.sprint_fov, &s.combat_fov, &s.aiming_fov, &s.traversal_fov})
        {
            *f = std::clamp(*f, -30.0, 30.0);
        }
        s.game_lag_scale = std::clamp(s.game_lag_scale, 1.0, 20.0);
        s.pitch_min = std::clamp(s.pitch_min, -89.0, -30.0);
        s.pitch_max = std::clamp(s.pitch_max, 20.0, 89.0);
        s.position_transition = std::clamp(s.position_transition, 0.0, 2.0);
    }

    inline auto parse_settings(const std::string& content) -> Settings
    {
        Settings s;
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = trim(line.substr(0, eq));
            auto value = trim(line.substr(eq + 1));
            if (!key.empty() && !value.empty()) set_value(s, key, value);
        }
        sanitize(s);
        return s;
    }

    inline auto number_of(const Settings& s, const std::string& key) -> double
    {
        if (key == "follow_rate_h") return s.follow_rate_h;
        if (key == "follow_rate_v") return s.follow_rate_v;
        if (key == "curve_h") return s.curve_h;
        if (key == "curve_v") return s.curve_v;
        if (key == "catchup_distance") return s.catchup_distance;
        if (key == "min_rate_scale") return s.min_rate_scale;
        if (key == "max_lag_h") return s.max_lag_h;
        if (key == "max_lag_v") return s.max_lag_v;
        if (key == "soft_leash") return s.soft_leash ? 1 : 0;
        if (key == "rotation_smoothing") return s.rotation_smoothing ? 1 : 0;
        if (key == "rotation_rate") return s.rotation_rate;
        if (key == "wall_clamp") return s.wall_clamp ? 1 : 0;
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

    inline auto apply_values(Settings& s, const Values& values) -> void
    {
        for (auto& [key, value] : values) set_value(s, key, format_number(value));
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

    // Temp file plus rename, so the Mod Menu never reads a half-written file.
    inline auto write_file(const std::string& path, const std::string& content) -> bool
    {
        auto tmp = path + ".dwsc.tmp";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file << content;
            if (!file) return false;
        }
        return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    struct NamedPreset
    {
        int id;
        const char* name;
        Values values;
    };

    // Menu ids 101-103, in cycle order. Balanced matches the shipped defaults.
    inline auto builtin_presets() -> const std::vector<NamedPreset>&
    {
        static const std::vector<NamedPreset> presets{
                {101, "Tight", {{"follow_rate_h", 18}, {"follow_rate_v", 20}, {"curve_h", 0}, {"curve_v", 0}, {"catchup_distance", 100},
                                {"min_rate_scale", 0.5}, {"max_lag_h", 25}, {"max_lag_v", 20}, {"soft_leash", 1},
                                {"rotation_smoothing", 0}, {"rotation_rate", 20}, {"wall_clamp", 1}}},
                {102, "Balanced", {{"follow_rate_h", 8}, {"follow_rate_v", 10}, {"curve_h", 2}, {"curve_v", 0}, {"catchup_distance", 150},
                                   {"min_rate_scale", 0.35}, {"max_lag_h", 70}, {"max_lag_v", 50}, {"soft_leash", 1},
                                   {"rotation_smoothing", 0}, {"rotation_rate", 20}, {"wall_clamp", 1}}},
                {103, "Cinematic", {{"follow_rate_h", 4}, {"follow_rate_v", 6}, {"curve_h", 3}, {"curve_v", 2}, {"catchup_distance", 200},
                                    {"min_rate_scale", 0.25}, {"max_lag_h", 120}, {"max_lag_v", 80}, {"soft_leash", 1},
                                    {"rotation_smoothing", 1}, {"rotation_rate", 25}, {"wall_clamp", 1}}},
        };
        return presets;
    }

    inline auto read_slots(const std::string& path) -> std::map<int, Values>
    {
        std::map<int, Values> slots;
        auto content = read_file(path);
        if (!content) return slots;
        std::istringstream in(*content);
        std::string line;
        int current = 0;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            line = trim(line);
            if (line.size() >= 7 && line.front() == '[' && line.back() == ']' && line.compare(1, 4, "Slot") == 0)
            {
                current = std::atoi(line.c_str() + 5);
                if (current < 1 || current > 6) current = 0;
                continue;
            }
            auto eq = line.find('=');
            if (!current || eq == std::string::npos) continue;
            // A hand-edited slot must not carry preset_load, which would re-trigger the load forever.
            auto key = trim(line.substr(0, eq));
            if (std::find_if(PRESET_KEYS.begin(), PRESET_KEYS.end(), [&](const char* k) { return key == k; }) == PRESET_KEYS.end()) continue;
            try
            {
                double v = std::stod(trim(line.substr(eq + 1)));
                if (std::isfinite(v)) slots[current].emplace_back(key, v);
            }
            catch (...)
            {
            }
        }
        return slots;
    }

    inline auto write_slots(const std::string& path, const std::map<int, Values>& slots) -> bool
    {
        std::string out = "; DWSmoothCam saved presets. Written by the mod when you save a slot from the Mod Menu.\n"
                          "; Load a slot from the menu, or cycle presets in game with preset_key.\n";
        for (auto& [slot, values] : slots)
        {
            out += "\n[Slot" + std::to_string(slot) + "]\n";
            for (auto& [key, value] : values) out += key + " = " + format_number(value) + "\n";
        }
        return write_file(path, out);
    }
} // namespace dwsc
