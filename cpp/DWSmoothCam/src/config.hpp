// DWSmoothCam settings and presets.
// scripts/config/smoothcam.ini holds the live values. The Dawnwalker Mod Menu rewrites its numbers in place
// (mod_settings.ini declares them), and the mod re-reads the file when it changes. Key names (toggle_key,
// preset_key) are read at startup only: the menu cannot move them.
// scripts/config/presets.ini holds the six user slots and is written only by the mod.
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

        double follow_rate_h = 8.0;       // 1/s: how fast the camera catches the character horizontally
        double follow_rate_v = 10.0;      // 1/s: vertically
        int curve_h = 2;                  // 0 exponential, 1 linear, 2 smoothstep, 3 ease in-out
        int curve_v = 0;
        double catchup_distance = 150.0;  // cm of lag at which a curve reaches the full rate
        double min_rate_scale = 0.35;     // rate multiplier at zero lag, for curves 1-3
        double max_lag_h = 70.0;          // cm
        double max_lag_v = 50.0;
        bool soft_leash = true;           // ease into max_lag instead of stopping hard at it

        bool rotation_smoothing = false;
        double rotation_rate = 20.0;

        bool wall_clamp = true;
        double reset_distance = 500.0;    // cm the character may move in one frame before the camera snaps
        double reset_gap = 0.25;          // s without a view update (cutscene, photo mode, load) before it snaps

        bool show_banner = true;          // announce presets and the toggle with the game's region banner

        int preset_load = 0;              // menu action: 101-103 built-in, 1-6 slot; the mod sets it back to 0
        int preset_save = 0;              // menu action: 1-6 slot; the mod sets it back to 0

        bool log_stats = false;
    };

    // The keys a preset carries: the feel, not the switches, keys or safety limits.
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
        auto number = [&](double& out) {
            try { out = std::stod(value); } catch (...) {}
        };
        auto integer = [&](int& out) {
            try { out = static_cast<int>(std::lround(std::stod(value))); } catch (...) {}
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
        else if (key == "preset_load") integer(s.preset_load);
        else if (key == "preset_save") integer(s.preset_save);
        else if (key == "log_stats") flag(s.log_stats);
    }

    inline auto sanitize(Settings& s) -> void
    {
        s.curve_h = std::clamp(s.curve_h, 0, 3);
        s.curve_v = std::clamp(s.curve_v, 0, 3);
        s.follow_rate_h = std::max(s.follow_rate_h, 0.0);
        s.follow_rate_v = std::max(s.follow_rate_v, 0.0);
        s.catchup_distance = std::max(s.catchup_distance, 1.0);
        s.min_rate_scale = std::clamp(s.min_rate_scale, 0.01, 1.0);
        s.max_lag_h = std::max(s.max_lag_h, 0.0);
        s.max_lag_v = std::max(s.max_lag_v, 0.0);
        s.rotation_rate = std::max(s.rotation_rate, 0.0);
        s.reset_distance = std::max(s.reset_distance, 1.0);
        s.reset_gap = std::max(s.reset_gap, 0.0);
    }

    // key = value lines; ';' and '#' start comments. Sections are ignored.
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

    // Replace the number of each key in place, keeping layout and comments, the way the Mod Menu does.
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

    // Temp file plus rename, so the Mod Menu never reads half a file.
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

    // Menu ids 101-103, cycled in this order. Balanced is the shipped default. Values follow PRESET_KEYS.
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

    // presets.ini: [Slot1] .. [Slot6], each with PRESET_KEYS.
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
            try
            {
                slots[current].emplace_back(trim(line.substr(0, eq)), std::stod(trim(line.substr(eq + 1))));
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
