// The settings schema: every key of smoothwalker.ini as a Settings field, the preset and numeric key lists, set_value
// and number_of by key name, the Mod Menu's ranges (sanitize) and apply_values. The file's text is ini.hpp's, the
// presets presets.hpp's. Key names are read at startup: the menu can only move numbers.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dw::smoothwalker::settings
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
        double combat_follow = 100.0;     // percent of the trail kept while a combat camera mode is blending in or active
        double traversal_follow = 100.0;  // percent of the trail kept while a traversal camera mode is blending in or active
        double focus_follow = 100.0;      // percent of the trail kept while a focus camera mode is blending in or active
        double interior_follow = 100.0;   // percent of the trail kept while the game's camera type is Interior

        bool rotation_smoothing = false;
        double rotation_rate = 20.0;
        double combat_rotation = 100.0;   // percent of the turning smoothing kept while a combat camera mode is blending in or active
        double traversal_rotation = 100.0; // percent of the turning smoothing kept while a traversal camera mode is blending in or active
        double focus_rotation = 100.0;    // percent of the turning smoothing kept while a focus camera mode is blending in or active
        double interior_rotation = 100.0; // percent of the turning smoothing kept while the game's camera type is Interior

        bool wall_clamp = true;
        double reset_distance = 500.0;    // cm moved in one frame that counts as a teleport
        double reset_gap = 0.25;          // s without a view update before the camera snaps

        bool show_banner = true;

        // Camera position (mode_tuner.hpp). These defaults leave the game's modes as shipped.
        bool camera_tuning = true;
        double exploration_distance = 100, exploration_height = 0, exploration_shoulder = 0, exploration_fov = 0;
        double sprint_distance = 100, sprint_height = 0, sprint_shoulder = 0, sprint_fov = 0;
        // The speed blend (follow/speed_blend.hpp): percent of the Sprint group's difference from the Exploration group's
        // that arrives with speed, from speed_blend_start to speed_blend_full cm/s (ini only; guessed: just above walk, sprint).
        // The speed eases through two stages of speed_blend_rise s each while it climbs and speed_blend_fall s while it drops
        // (ini only; tuned on a keyboard run 2026-09-30: a start in about 2.4 s, a stop settled in about 1.2 s).
        double speed_blend = 0, speed_blend_start = 150, speed_blend_full = 558, speed_blend_rise = 0.35, speed_blend_fall = 0.75;
        double combat_distance = 100, combat_height = 0, combat_shoulder = 0, combat_fov = 0;
        double focus_distance = 100, focus_height = 0, focus_shoulder = 0, focus_fov = 0; // 0.9.0; a file without them takes combat's
        double aiming_distance = 100, aiming_height = 0, aiming_shoulder = 0, aiming_fov = 0;
        double traversal_distance = 100, traversal_height = 0, traversal_fov = 0;
        // On top of the groups above, on the camera type the game selects inside buildings (modes/position.hpp, InteriorTuning).
        double interior_distance = 100, interior_height = 0, interior_shoulder = 0, interior_fov = 0;
        bool shoulder_swap = false;
        double pitch_min = -60, pitch_max = 40;
        double position_transition = 0.5; // s; 0 snaps

        int preset = 102;                 // the preset the live settings match: 0 Custom, 101-103 built-in, 1-MAX_SLOTS slot, FIRST_DROPIN_ID on drop-in

        bool log_stats = false;
        bool log_verbose = false; // discovery, offsets, layouts, camera mode changes in the log; ini only, not on the Mod Menu page

        bool debug_overlay = false; // the live panel at the top right of the screen (debug_overlay.hpp)
        bool debug_markers = false; // the camera's trail drawn on the ground around the character (marker_layer.hpp)
        std::string debug_key;      // shows and hides both; empty: not bound
    };

    // A preset is a camera look: follow, turning, the look limits and camera position. Not the switches (enabled,
    // camera_tuning, shoulder_swap, show_banner, log_stats, log_verbose, debug_overlay, debug_markers), the safety values
    // (wall_clamp, reset_distance, reset_gap), position_transition, the key names, or preset.
    inline const std::array<const char*, 50> PRESET_KEYS{
            "follow_rate_h", "follow_rate_v", "curve_h", "curve_v", "catchup_distance", "min_rate_scale", "max_lag_h", "max_lag_v",
            "soft_leash", "aiming_follow", "combat_follow", "traversal_follow", "focus_follow", "interior_follow", "rotation_smoothing",
            "rotation_rate", "combat_rotation", "traversal_rotation", "focus_rotation", "interior_rotation", "pitch_min", "pitch_max", "exploration_distance", "exploration_height", "exploration_shoulder",
            "exploration_fov", "sprint_distance", "sprint_height", "sprint_shoulder", "sprint_fov", "speed_blend", "combat_distance", "combat_height",
            "combat_shoulder", "combat_fov", "focus_distance", "focus_height", "focus_shoulder", "focus_fov", "aiming_distance", "aiming_height", "aiming_shoulder", "aiming_fov", "traversal_distance",
            "traversal_height", "traversal_fov", "interior_distance", "interior_height", "interior_shoulder", "interior_fov"};

    // Every numeric setting: the ones the Mod Menu can move and the mod writes back.
    inline const std::array<const char*, 67> NUMERIC_KEYS{
            "enabled", "follow_rate_h", "follow_rate_v", "curve_h", "curve_v", "catchup_distance", "min_rate_scale", "max_lag_h",
            "max_lag_v", "soft_leash", "aiming_follow", "combat_follow", "traversal_follow", "focus_follow", "interior_follow", "rotation_smoothing",
            "rotation_rate", "combat_rotation", "traversal_rotation", "focus_rotation", "interior_rotation", "wall_clamp", "reset_distance", "reset_gap", "show_banner",
            "camera_tuning", "exploration_distance", "exploration_height", "exploration_shoulder", "exploration_fov", "sprint_distance",
            "sprint_height", "sprint_shoulder", "sprint_fov", "speed_blend", "speed_blend_start", "speed_blend_full", "speed_blend_rise", "speed_blend_fall", "combat_distance", "combat_height", "combat_shoulder", "combat_fov", "focus_distance", "focus_height", "focus_shoulder", "focus_fov",
            "aiming_distance", "aiming_height", "aiming_shoulder", "aiming_fov", "traversal_distance", "traversal_height", "traversal_fov",
            "interior_distance", "interior_height", "interior_shoulder", "interior_fov", "shoulder_swap", "pitch_min", "pitch_max", "position_transition", "preset", "log_stats", "log_verbose", "debug_overlay",
            "debug_markers"};

    inline auto is_preset_key(const std::string& key) -> bool
    {
        return std::find_if(PRESET_KEYS.begin(), PRESET_KEYS.end(), [&](const char* k) { return key == k; }) != PRESET_KEYS.end();
    }

    inline auto is_numeric_key(const std::string& key) -> bool
    {
        return std::find_if(NUMERIC_KEYS.begin(), NUMERIC_KEYS.end(), [&](const char* k) { return key == k; }) != NUMERIC_KEYS.end();
    }

    using Values = std::vector<std::pair<std::string, double>>;

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
        else if (key == "combat_follow") number(s.combat_follow);
        else if (key == "traversal_follow") number(s.traversal_follow);
        else if (key == "focus_follow") number(s.focus_follow);
        else if (key == "interior_follow") number(s.interior_follow);
        else if (key == "rotation_smoothing") flag(s.rotation_smoothing);
        else if (key == "rotation_rate") number(s.rotation_rate);
        else if (key == "combat_rotation") number(s.combat_rotation);
        else if (key == "traversal_rotation") number(s.traversal_rotation);
        else if (key == "focus_rotation") number(s.focus_rotation);
        else if (key == "interior_rotation") number(s.interior_rotation);
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
        else if (key == "speed_blend") number(s.speed_blend);
        else if (key == "speed_blend_start") number(s.speed_blend_start);
        else if (key == "speed_blend_full") number(s.speed_blend_full);
        else if (key == "speed_blend_rise") number(s.speed_blend_rise);
        else if (key == "speed_blend_fall") number(s.speed_blend_fall);
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
        else if (key == "interior_distance") number(s.interior_distance);
        else if (key == "interior_height") number(s.interior_height);
        else if (key == "interior_shoulder") number(s.interior_shoulder);
        else if (key == "interior_fov") number(s.interior_fov);
        else if (key == "pitch_min") number(s.pitch_min);
        else if (key == "pitch_max") number(s.pitch_max);
        else if (key == "preset") integer(s.preset);
        else if (key == "log_stats") flag(s.log_stats);
        else if (key == "log_verbose") flag(s.log_verbose);
        else if (key == "debug_overlay") flag(s.debug_overlay);
        else if (key == "debug_markers") flag(s.debug_markers);
        else if (key == "debug_key") s.debug_key = value;
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
        s.combat_follow = std::clamp(s.combat_follow, 0.0, 100.0);
        s.traversal_follow = std::clamp(s.traversal_follow, 0.0, 100.0);
        s.focus_follow = std::clamp(s.focus_follow, 0.0, 100.0);
        s.interior_follow = std::clamp(s.interior_follow, 0.0, 100.0);
        s.rotation_rate = std::clamp(s.rotation_rate, 1.0, 60.0);
        s.combat_rotation = std::clamp(s.combat_rotation, 0.0, 100.0);
        s.traversal_rotation = std::clamp(s.traversal_rotation, 0.0, 100.0);
        s.focus_rotation = std::clamp(s.focus_rotation, 0.0, 100.0);
        s.interior_rotation = std::clamp(s.interior_rotation, 0.0, 100.0);
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
        s.speed_blend = std::clamp(s.speed_blend, 0.0, 100.0);
        s.speed_blend_start = std::clamp(s.speed_blend_start, 0.0, 2000.0); // ini only: no page range; haste is about 900 cm/s
        s.speed_blend_full = std::clamp(s.speed_blend_full, 0.0, 2000.0);
        s.speed_blend_rise = std::clamp(s.speed_blend_rise, 0.05, 5.0);
        s.speed_blend_fall = std::clamp(s.speed_blend_fall, 0.05, 5.0);
        s.interior_distance = std::clamp(s.interior_distance, 50.0, 200.0);
        s.interior_height = std::clamp(s.interior_height, -50.0, 50.0);
        s.interior_shoulder = std::clamp(s.interior_shoulder, -60.0, 60.0);
        s.interior_fov = std::clamp(s.interior_fov, -30.0, 30.0);
        s.pitch_min = std::clamp(s.pitch_min, -89.0, -30.0);
        s.pitch_max = std::clamp(s.pitch_max, 20.0, 89.0);
        s.position_transition = std::clamp(s.position_transition, 0.0, 2.0);
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
        if (key == "combat_follow") return s.combat_follow;
        if (key == "traversal_follow") return s.traversal_follow;
        if (key == "focus_follow") return s.focus_follow;
        if (key == "interior_follow") return s.interior_follow;
        if (key == "rotation_smoothing") return flag(s.rotation_smoothing);
        if (key == "rotation_rate") return s.rotation_rate;
        if (key == "combat_rotation") return s.combat_rotation;
        if (key == "traversal_rotation") return s.traversal_rotation;
        if (key == "focus_rotation") return s.focus_rotation;
        if (key == "interior_rotation") return s.interior_rotation;
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
        if (key == "speed_blend") return s.speed_blend;
        if (key == "speed_blend_start") return s.speed_blend_start;
        if (key == "speed_blend_full") return s.speed_blend_full;
        if (key == "speed_blend_rise") return s.speed_blend_rise;
        if (key == "speed_blend_fall") return s.speed_blend_fall;
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
        if (key == "interior_distance") return s.interior_distance;
        if (key == "interior_height") return s.interior_height;
        if (key == "interior_shoulder") return s.interior_shoulder;
        if (key == "interior_fov") return s.interior_fov;
        if (key == "shoulder_swap") return flag(s.shoulder_swap);
        if (key == "pitch_min") return s.pitch_min;
        if (key == "pitch_max") return s.pitch_max;
        if (key == "position_transition") return s.position_transition;
        if (key == "preset") return s.preset;
        if (key == "log_stats") return flag(s.log_stats);
        if (key == "log_verbose") return flag(s.log_verbose);
        if (key == "debug_overlay") return flag(s.debug_overlay);
        if (key == "debug_markers") return flag(s.debug_markers);
        return 0.0;
    }

    inline auto preset_of(const Settings& s) -> Values
    {
        Values out;
        for (auto* key : PRESET_KEYS) out.emplace_back(key, number_of(s, key));
        return out;
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
} // namespace dw::smoothwalker::settings
