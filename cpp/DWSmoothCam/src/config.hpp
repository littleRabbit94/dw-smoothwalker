// DWSmoothCam settings: scripts/config/smoothcam.ini, read at startup. Numeric keys only, so the
// Dawnwalker Mod Menu can drive them later; the toggle key is a key name.
#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace dwsc
{
    struct Settings
    {
        bool enabled = true;
        std::string toggle_key = "O";

        double follow_rate_h = 6.0;   // 1/s: how fast the camera catches the character horizontally
        double follow_rate_v = 10.0;  // 1/s: vertically
        int curve_h = 2;              // 0 exponential, 1 linear, 2 smoothstep, 3 ease in-out
        int curve_v = 0;
        double catchup_distance = 150.0; // cm of lag at which a curve reaches the full rate
        double min_rate_scale = 0.25;    // rate multiplier at zero lag, for curves 1-3
        double max_lag_h = 80.0;         // cm, hard leash
        double max_lag_v = 60.0;

        bool rotation_smoothing = false;
        double rotation_rate = 20.0;

        bool wall_clamp = true;
        double reset_distance = 500.0; // cm the character may move in one frame before the camera snaps
        double reset_gap = 0.25;       // s without a view update (cutscene, photo mode, load) before it snaps

        bool log_stats = false;
    };

    inline Settings load_settings(const char* path)
    {
        Settings s;
        std::ifstream file(path);
        if (!file) return s;

        std::string line;
        while (std::getline(file, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string v) {
                auto not_space = [](unsigned char c) { return !std::isspace(c); };
                v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
                v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
                return v;
            };
            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));
            if (key.empty() || value.empty()) continue;

            auto number = [&](double& out) {
                try { out = std::stod(value); } catch (...) {}
            };
            auto integer = [&](int& out) {
                try { out = std::stoi(value); } catch (...) {}
            };
            auto flag = [&](bool& out) { out = value == "1" || value == "true" || value == "True"; };

            if (key == "enabled") flag(s.enabled);
            else if (key == "toggle_key") s.toggle_key = value;
            else if (key == "follow_rate_h") number(s.follow_rate_h);
            else if (key == "follow_rate_v") number(s.follow_rate_v);
            else if (key == "curve_h") integer(s.curve_h);
            else if (key == "curve_v") integer(s.curve_v);
            else if (key == "catchup_distance") number(s.catchup_distance);
            else if (key == "min_rate_scale") number(s.min_rate_scale);
            else if (key == "max_lag_h") number(s.max_lag_h);
            else if (key == "max_lag_v") number(s.max_lag_v);
            else if (key == "rotation_smoothing") flag(s.rotation_smoothing);
            else if (key == "rotation_rate") number(s.rotation_rate);
            else if (key == "wall_clamp") flag(s.wall_clamp);
            else if (key == "reset_distance") number(s.reset_distance);
            else if (key == "reset_gap") number(s.reset_gap);
            else if (key == "log_stats") flag(s.log_stats);
        }

        s.curve_h = std::clamp(s.curve_h, 0, 3);
        s.curve_v = std::clamp(s.curve_v, 0, 3);
        s.min_rate_scale = std::clamp(s.min_rate_scale, 0.01, 1.0);
        s.max_lag_h = std::max(s.max_lag_h, 0.0);
        s.max_lag_v = std::max(s.max_lag_v, 0.0);
        return s;
    }
} // namespace dwsc
