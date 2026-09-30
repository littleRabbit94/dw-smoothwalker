// smoothwalker/settings: the schema (set_value, number_of, sanitize) and smoothwalker.ini as text (ini.hpp): parse,
// the diff baseline, the in-place rewrite, the missing-keys writer, and the file read and write.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "check.hpp"
#include "smoothwalker/settings/ini.hpp"
#include "smoothwalker/settings/settings.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace dw::smoothwalker::settings;

namespace
{
    auto contains(const std::string& s, const std::string& part) -> bool { return s.find(part) != std::string::npos; }
    auto spaces(size_t n) -> std::string { return std::string(n, ' '); }
} // namespace

TEST(ini, every_numeric_key_round_trips)
{
    // set_value and number_of name the same keys: every NUMERIC_KEYS entry is written and read back.
    for (const char* key : NUMERIC_KEYS)
    {
        Settings s;
        set_value(s, key, "1");
        CHECK_EQ(number_of(s, key), 1.0);
    }
    for (const char* key : PRESET_KEYS) CHECK(is_numeric_key(key));
    CHECK(!is_numeric_key("toggle_key"));
    CHECK(!is_preset_key("enabled"));
    CHECK(!is_preset_key("position_transition"));
    CHECK_EQ(number_of(Settings{}, "no_such_key"), 0.0);
}

TEST(ini, set_value_forms)
{
    Settings s;
    set_value(s, "follow_rate_h", "8.25");
    CHECK_EQ(s.follow_rate_h, 8.25);
    set_value(s, "follow_rate_h", "nan"); // std::stod takes nan and inf: ignored
    set_value(s, "follow_rate_h", "inf");
    set_value(s, "follow_rate_h", "abc");
    CHECK_EQ(s.follow_rate_h, 8.25);
    set_value(s, "curve_h", "1.6"); // rounded
    CHECK_EQ(s.curve_h, 2);
    set_value(s, "curve_h", "1e7"); // out of int range: ignored
    CHECK_EQ(s.curve_h, 2);
    set_value(s, "soft_leash", "0");
    CHECK(!s.soft_leash);
    set_value(s, "soft_leash", "true"); // not a number: the words
    CHECK(s.soft_leash);
    set_value(s, "soft_leash", "yes");
    CHECK(!s.soft_leash);
    set_value(s, "soft_leash", "0.2"); // any non-zero number is on
    CHECK(s.soft_leash);
    set_value(s, "toggle_key", "F5");
    CHECK_EQ(s.toggle_key, std::string("F5"));
}

TEST(ini, parse_keeps_defaults_and_strips_comments)
{
    Settings s = parse_settings("; a comment\n# another\nfollow_rate_h = 8 ; trailing\nmax_lag_h=40#x\n\n  curve_v   =   3  \r\nno_equals_line\nunknown = 5\n");
    CHECK_EQ(s.follow_rate_h, 8.0);
    CHECK_EQ(s.max_lag_h, 40.0);
    CHECK_EQ(s.curve_v, 3);
    CHECK_EQ(s.follow_rate_v, 10.0);
    CHECK_EQ(s.shoulder_key, std::string("V"));
    Settings d = parse_settings("");
    CHECK_EQ(d.preset, 102);
    CHECK(d.enabled);
}

TEST(ini, blank_key_names_unbind)
{
    Settings s = parse_settings("shoulder_key =\ntoggle_key = O\ndebug_key = ; blank\nfollow_rate_h =\n");
    CHECK_EQ(s.shoulder_key, std::string()); // unbound, not the default V
    CHECK_EQ(s.toggle_key, std::string("O"));
    CHECK_EQ(s.debug_key, std::string());
    CHECK_EQ(s.follow_rate_h, 6.5); // a blank number keeps the default
}

TEST(ini, nan_and_inf_ignored)
{
    Settings s = parse_settings("follow_rate_h = nan\nmax_lag_h = inf\nmax_lag_v = -inf\ncurve_h = nan\n");
    CHECK_EQ(s.follow_rate_h, 6.5);
    CHECK_EQ(s.max_lag_h, 85.0);
    CHECK_EQ(s.max_lag_v, 50.0);
    CHECK_EQ(s.curve_h, 2);
    auto numbers = parse_numbers("follow_rate_h = nan\nmax_lag_h = inf\n");
    CHECK(numbers.empty());
}

TEST(ini, focus_fallback)
{
    // A file from before 0.9.0: focus takes combat's values.
    Settings s = parse_settings("combat_distance = 120\ncombat_height = 10\ncombat_shoulder = 5\ncombat_fov = 3\n");
    CHECK_EQ(s.focus_distance, 120.0);
    CHECK_EQ(s.focus_height, 10.0);
    CHECK_EQ(s.focus_shoulder, 5.0);
    CHECK_EQ(s.focus_fov, 3.0);
    // Any focus key with a value: no fallback, the others keep their defaults.
    s = parse_settings("combat_distance = 120\nfocus_fov = 2\n");
    CHECK_EQ(s.focus_distance, 100.0);
    CHECK_EQ(s.focus_fov, 2.0);
    // A blank focus key does not count.
    s = parse_settings("combat_distance = 120\nfocus_fov =\n");
    CHECK_EQ(s.focus_distance, 120.0);
    // Combat clamped, focus too: the fallback runs before sanitize.
    s = parse_settings("combat_distance = 900\n");
    CHECK_EQ(s.focus_distance, 250.0);
}

TEST(ini, sanitize_ranges)
{
    Settings s = parse_settings("follow_rate_h = 100\nfollow_rate_v = 0\ncurve_h = 9\ncurve_v = -2\ncatchup_distance = 1\n"
                                "min_rate_scale = 2\nmax_lag_h = 999\nmax_lag_v = -1\naiming_follow = 150\nrotation_rate = 0\n"
                                "reset_distance = 5\nreset_gap = 9\nexploration_distance = 10\nsprint_height = 500\n"
                                "aiming_shoulder = -100\ntraversal_fov = 45\npitch_min = 0\npitch_max = 95\nposition_transition = -1\n");
    CHECK_EQ(s.follow_rate_h, 30.0);
    CHECK_EQ(s.follow_rate_v, 0.5);
    CHECK_EQ(s.curve_h, 3);
    CHECK_EQ(s.curve_v, 0);
    CHECK_EQ(s.catchup_distance, 10.0);
    CHECK_EQ(s.min_rate_scale, 1.0);
    CHECK_EQ(s.max_lag_h, 300.0);
    CHECK_EQ(s.max_lag_v, 0.0);
    CHECK_EQ(s.aiming_follow, 100.0);
    CHECK_EQ(s.rotation_rate, 1.0);
    CHECK_EQ(s.reset_distance, 100.0);
    CHECK_EQ(s.reset_gap, 2.0);
    CHECK_EQ(s.exploration_distance, 25.0);
    CHECK_EQ(s.sprint_height, 100.0);
    CHECK_EQ(s.aiming_shoulder, -60.0);
    CHECK_EQ(s.traversal_fov, 30.0);
    CHECK_EQ(s.pitch_min, -30.0);
    CHECK_EQ(s.pitch_max, 89.0);
    CHECK_EQ(s.position_transition, 0.0);
    Settings d;
    Settings before = d;
    sanitize(d); // the defaults are in range
    CHECK_EQ(d.follow_rate_h, before.follow_rate_h);
    CHECK_EQ(d.pitch_min, before.pitch_min);
}

TEST(ini, parse_numbers_last_wins_unsanitized)
{
    auto n = parse_numbers("follow_rate_h = 5\nfollow_rate_h = 7\nmax_lag_h = 999 ; kept as written\ntoggle_key = 3\nfoo = 1\n"
                           "; curve_h = 1\ncurve_v = x\n");
    CHECK_EQ(n.size(), 2u);
    CHECK_EQ(n["follow_rate_h"], 7.0);
    CHECK_EQ(n["max_lag_h"], 999.0);
    CHECK(n.find("toggle_key") == n.end()); // a key name, not a number
    CHECK(n.find("curve_h") == n.end());
    CHECK(n.find("curve_v") == n.end());
    CHECK_EQ(parse_settings("follow_rate_h = 5\nfollow_rate_h = 7\n").follow_rate_h, 7.0);
}

TEST(ini, format_number)
{
    CHECK_EQ(format_number(6.5), std::string("6.5"));
    CHECK_EQ(format_number(100.0), std::string("100"));
    CHECK_EQ(format_number(0.35), std::string("0.35"));
    CHECK_EQ(format_number(1.0 / 3.0), std::string("0.333333"));
    CHECK_EQ(format_number(-60.0), std::string("-60"));
    CHECK_EQ(format_number(1e-7), std::string("1e-07"));
}

TEST(ini, apply_values_full_precision)
{
    Settings s;
    apply_values(s, {{"follow_rate_h", 6.1 + 0.2}, {"curve_h", 2.6}, {"max_lag_h", 1000.0}});
    CHECK_EQ(s.follow_rate_h, 6.1 + 0.2); // not 6.3: %.17g keeps every bit
    CHECK_EQ(s.curve_h, 3);
    CHECK_EQ(s.max_lag_h, 300.0); // sanitized
    Values v = preset_of(Settings{});
    CHECK_EQ(v.size(), PRESET_KEYS.size());
    CHECK_EQ(v.front().first, std::string("follow_rate_h"));
    CHECK_EQ(v.front().second, 6.5);
}

TEST(ini, rewrite_keeps_layout_crlf_and_comments)
{
    const std::string in = "; Smoothwalker\r\n"
                           "follow_rate_h    = 6.5    ; horizontal catch-up rate\r\n"
                           "; follow_rate_v = 10\r\n"
                           "follow_rate_v=10\r\n"
                           "max_lag_h =\r\n"
                           "# max_lag_v = 50\r\n"
                           "max_lag_v = 50 # vertical\r\n"
                           "curve_h = 2\r\n";
    auto out = rewrite_numbers(in, {{"follow_rate_h", 12.0}, {"follow_rate_v", 1.0 / 3.0}, {"max_lag_h", 40.0}, {"max_lag_v", 20.0}});
    CHECK_EQ(out, std::string("; Smoothwalker\r\n"
                              "follow_rate_h    = 12    ; horizontal catch-up rate\r\n"
                              "; follow_rate_v = 10\r\n"
                              "follow_rate_v=0.333333\r\n"
                              "max_lag_h =\r\n"
                              "# max_lag_v = 50\r\n"
                              "max_lag_v = 20 # vertical\r\n"
                              "curve_h = 2\r\n"));
    CHECK_EQ(rewrite_numbers(in, {}), in);
}

TEST(ini, rewrite_final_newline)
{
    CHECK_EQ(rewrite_numbers("a = 1\nb = 2", {{"b", 3.0}}), std::string("a = 1\nb = 3"));
    CHECK_EQ(rewrite_numbers("a = 1\nb = 2\n", {{"a", 5.0}}), std::string("a = 5\nb = 2\n"));
    CHECK_EQ(rewrite_numbers("", {{"a", 5.0}}), std::string());
    // Repeated keys: every assignment line takes the value.
    CHECK_EQ(rewrite_numbers("a = 1\na = 2\n", {{"a", 5.0}}), std::string("a = 5\na = 5\n"));
}

TEST(ini, shipped_comments)
{
    CHECK_EQ(std::string(shipped_comment("curve_h")), std::string("0 constant, 1 linear, 2 smoothstep, 3 ease in-out"));
    CHECK(shipped_comment("curve_v") == nullptr); // none in the shipped file
    CHECK(shipped_comment("debug_key") != nullptr);
    CHECK(shipped_comment("toggle_key") == nullptr);
}

TEST(ini, missing_keys_none)
{
    std::string full;
    for (const char* key : NUMERIC_KEYS) full += std::string(key) + " = 1\n";
    full += "debug_key =\n";
    auto [out, added] = with_missing_keys(full, Settings{});
    CHECK(added.empty());
    CHECK_EQ(out, full);
}

TEST(ini, missing_keys_empty_file)
{
    auto [out, added] = with_missing_keys("", Settings{});
    CHECK_EQ(added.size(), NUMERIC_KEYS.size() + 1);
    CHECK_EQ(added.front(), std::string("enabled"));
    CHECK_EQ(added.back(), std::string("debug_key"));
    CHECK_EQ(added[added.size() - 2], std::string("debug_markers"));
    CHECK_EQ(added[added.size() - 3], std::string("debug_overlay"));
    // Laid out as the shipped file: the key padded to 16, the value to 6, then its comment.
    CHECK(out.starts_with("enabled" + spaces(9) + " = 1" + spaces(5) + "; 0 is the game's own camera"));
    CHECK(contains(out, "\ncurve_v" + spaces(9) + " = 0\n"));                        // no shipped comment: none
    CHECK(contains(out, "\nexploration_distance = 100" + spaces(3) + "; percent of")); // a long key: no padding
    CHECK(contains(out, "\nmin_rate_scale" + spaces(2) + " = 0.35" + spaces(2) + "; rate multiplier"));
    CHECK(contains(out, "\ndebug_key" + spaces(7) + " = " + spaces(6) + "; shows and hides"));
    CHECK(out.ends_with("\n"));
    CHECK(!contains(out, "\r"));
    // The added text parses back to the live values.
    auto numbers = parse_numbers(out);
    CHECK_EQ(numbers.size(), NUMERIC_KEYS.size());
    CHECK_EQ(numbers["follow_rate_h"], 6.5);
}

TEST(ini, missing_keys_placement)
{
    Settings live;
    live.follow_rate_h = 9;
    const std::string in = "; head\nenabled = 1 ; on\nfollow_rate_v = 10\n; tail\n";
    auto [out, added] = with_missing_keys(in, live);
    CHECK_EQ(added.size(), NUMERIC_KEYS.size() - 2 + 1);
    // follow_rate_h after enabled, the nearest earlier key the file has; the rest after follow_rate_v, in order.
    const std::string head =
            "; head\nenabled = 1 ; on\nfollow_rate_h" + spaces(3) + " = 9" + spaces(5) + "; horizontal catch-up rate\nfollow_rate_v = 10\ncurve_h ";
    CHECK(out.starts_with(head));
    CHECK(out.ends_with("\n; tail\n"));
    auto overlay = out.find("\ndebug_overlay "), markers = out.find("\ndebug_markers "), key = out.find("\ndebug_key "),
         curve = out.find("\ncurve_h ");
    CHECK(overlay != std::string::npos && markers == out.find('\n', overlay + 1)); // debug_markers right after debug_overlay
    CHECK(markers != std::string::npos && key == out.find('\n', markers + 1));     // debug_key right after debug_markers
    CHECK(curve < overlay);
}

TEST(ini, missing_keys_crlf_and_no_final_newline)
{
    auto [out, added] = with_missing_keys("enabled = 1\r\nfollow_rate_v = 10", Settings{});
    CHECK(out.starts_with("enabled = 1\r\nfollow_rate_h" + spaces(3) + " = 6.5" + spaces(3) + "; horizontal catch-up rate\r\nfollow_rate_v = 10\r\ncurve_h "));
    CHECK(!out.ends_with("\n")); // a missing final newline stays missing
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i] == '\n') CHECK(i > 0 && out[i - 1] == '\r');
    }
}

TEST(ini, missing_keys_what_counts)
{
    // A commented-out key is missing; one with a bad value is there (the flush rewrites it).
    auto [out, added] = with_missing_keys("; follow_rate_h = 3\nfollow_rate_v = abc\n", Settings{});
    CHECK(std::find(added.begin(), added.end(), "follow_rate_h") != added.end());
    CHECK(std::find(added.begin(), added.end(), "follow_rate_v") == added.end());
    CHECK(std::find(added.begin(), added.end(), "enabled") != added.end());
    // Keys before the first one the file has go after the last line.
    CHECK(out.starts_with("; follow_rate_h = 3\nfollow_rate_v = abc\nenabled "));
}

namespace
{
    struct TempDir
    {
        std::string path;
        TempDir()
        {
            char base[MAX_PATH]{};
            GetTempPathA(MAX_PATH, base);
            path = std::string(base) + "dw_unit_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
            CreateDirectoryA(path.c_str(), nullptr);
        }
        ~TempDir()
        {
            WIN32_FIND_DATAA data{};
            HANDLE find = FindFirstFileA((path + "\\*").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    std::string name = data.cFileName;
                    if (name == "." || name == "..") continue;
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryA((path + "\\" + name).c_str());
                    else DeleteFileA((path + "\\" + name).c_str());
                } while (FindNextFileA(find, &data));
                FindClose(find);
            }
            RemoveDirectoryA(path.c_str());
        }
    };
} // namespace

TEST(ini, read_and_write_file)
{
    TempDir dir;
    const std::string file = dir.path + "\\smoothwalker.ini";
    CHECK(!read_file(file));
    CHECK(write_file(file, "a = 1\r\nb = 2"));
    CHECK_EQ(read_file(file).value_or(""), std::string("a = 1\r\nb = 2"));
    CHECK(write_file(file, "c = 3\n")); // replaces
    CHECK_EQ(read_file(file).value_or(""), std::string("c = 3\n"));
    CHECK(!read_file(file + ".dwsc.tmp")); // no temp file left
    CHECK(!write_file(dir.path + "\\missing\\x.ini", "x")); // no folder: fails, nothing left behind
    CHECK(!read_file(dir.path + "\\missing\\x.ini.dwsc.tmp"));
}
