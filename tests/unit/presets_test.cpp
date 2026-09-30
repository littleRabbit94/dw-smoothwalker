// smoothwalker/settings/presets: the built-ins, the preset file parse, normalization, labels, the slot file, the
// Preset picker's manifest lines, the folder listing, and the UTF-8 helpers (common/text.hpp) the labels use.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "check.hpp"
#include "common/text.hpp"
#include "smoothwalker/settings/ini.hpp"
#include "smoothwalker/settings/presets.hpp"
#include "smoothwalker/settings/settings.hpp"

#include <algorithm>
#include <fstream>
#include <string>

using namespace dw::smoothwalker::settings;

namespace
{
    auto value_of(const Values& values, const std::string& key) -> const double*
    {
        auto it = std::find_if(values.begin(), values.end(), [&](auto& e) { return e.first == key; });
        return it == values.end() ? nullptr : &it->second;
    }
} // namespace

TEST(presets, builtins_in_cycle_order_with_every_key)
{
    const auto& b = builtin_presets();
    CHECK_EQ(b.size(), 3u);
    CHECK_EQ(b[0].id, 101);
    CHECK_EQ(std::string(b[0].name), std::string("Tight"));
    CHECK_EQ(b[1].id, 102);
    CHECK_EQ(std::string(b[1].name), std::string("Balanced"));
    CHECK_EQ(b[2].id, 103);
    CHECK_EQ(std::string(b[2].name), std::string("Cinematic"));
    for (auto& p : b)
    {
        CHECK_EQ(p.values.size(), PRESET_KEYS.size());
        for (size_t i = 0; i < PRESET_KEYS.size() && i < p.values.size(); ++i) CHECK_EQ(p.values[i].first, std::string(PRESET_KEYS[i]));
    }
}

TEST(presets, builtins_normalize_to_themselves)
{
    // Inside every Mod Menu range, integers where the key is one, and exact in %.6g.
    for (auto& p : builtin_presets())
    {
        Values n = normalize_preset(p.values);
        CHECK_EQ(n.size(), p.values.size());
        for (size_t i = 0; i < n.size() && i < p.values.size(); ++i)
        {
            CHECK_EQ(n[i].first, p.values[i].first);
            CHECK_EQ(n[i].second, p.values[i].second);
        }
    }
}

TEST(presets, balanced_is_the_default)
{
    const Settings d;
    const auto& balanced = builtin_presets()[1];
    for (const char* key : PRESET_KEYS)
    {
        const double* v = value_of(balanced.values, key);
        CHECK(v != nullptr);
        if (v) CHECK_EQ(*v, number_of(d, key));
    }
    CHECK_EQ(d.preset, balanced.id);
}

TEST(presets, parse_file_bom_name_and_values)
{
    auto [name, values] = parse_preset_file("\xEF\xBB\xBF; a comment\r\nname = My Look ; trailing\r\nfollow_rate_h = 8\r\n"
                                            "enabled = 0\r\nmax_lag_h = nan\r\nfollow_rate_v = 12\r\nfollow_rate_h = 9\r\ncurve_h = x\r\n");
    CHECK_EQ(name, std::string("My Look"));
    CHECK_EQ(values.size(), 2u); // enabled is no preset key; nan and x are skipped
    CHECK_EQ(values[0].first, std::string("follow_rate_h"));
    CHECK_EQ(values[0].second, 9.0); // a repeated key: its first place, its last value
    CHECK_EQ(values[1].first, std::string("follow_rate_v"));
    auto [unnamed, none] = parse_preset_file("");
    CHECK(unnamed.empty());
    CHECK(none.empty());
    auto [raw, big] = parse_preset_file("follow_rate_h = 100\n");
    CHECK_EQ(big[0].second, 100.0); // raw: normalize_preset clamps
}

TEST(presets, fill_focus)
{
    Values v{{"combat_distance", 120}, {"combat_fov", 3}, {"focus_fov", 7}};
    fill_focus(v);
    CHECK_EQ(v.size(), 4u);
    CHECK(value_of(v, "focus_distance") && *value_of(v, "focus_distance") == 120.0);
    CHECK_EQ(*value_of(v, "focus_fov"), 7.0);  // present: kept
    CHECK(value_of(v, "focus_height") == nullptr); // no combat_height to take
    auto [name, parsed] = parse_preset_file("combat_height = 10\n");
    CHECK(value_of(parsed, "focus_height") && *value_of(parsed, "focus_height") == 10.0);
}

TEST(presets, normalize_clamps_rounds_and_formats)
{
    Values n = normalize_preset({{"follow_rate_h", 100}, {"curve_h", 1.4}, {"soft_leash", 0.3}, {"max_lag_h", 0.1234567}, {"pitch_min", 0}});
    CHECK_EQ(n.size(), 5u);
    CHECK_EQ(n[0].second, 30.0);
    CHECK_EQ(n[1].second, 1.0);
    CHECK_EQ(n[2].second, 1.0);
    CHECK_EQ(n[3].second, 0.123457);
    CHECK_EQ(n[4].second, -30.0);
    CHECK_EQ(n[4].first, std::string("pitch_min")); // keys and order as given
}

TEST(presets, clean_label)
{
    CHECK_EQ(clean_label("  My|Look\t\x01 "), std::string("MyLook"));
    CHECK_EQ(clean_label(std::string(60, 'a')), std::string(48, 'a'));
    // 47 bytes then a two-byte code point: cut before it, never inside.
    CHECK_EQ(clean_label(std::string(47, 'a') + "\xC3\xA9" + "bc"), std::string(47, 'a'));
    CHECK_EQ(clean_label(std::string(46, 'a') + "\xC3\xA9" + "bc"), std::string(46, 'a') + "\xC3\xA9");
    CHECK_EQ(clean_label(std::string(47, 'a') + " bcd"), std::string(47, 'a')); // trimmed after the cut
    CHECK_EQ(clean_label("bad \xFF byte"), std::string());                      // invalid UTF-8: nothing
    CHECK_EQ(clean_label("Caf\xC3\xA9"), std::string("Caf\xC3\xA9"));
    CHECK_EQ(clean_label("a\x7F" "b"), std::string("ab"));
}

TEST(presets, display_name)
{
    CHECK_EQ(display_name("Night Ride", "file", 201), std::string("Night Ride"));
    CHECK_EQ(display_name("", "file stem", 201), std::string("file stem"));
    CHECK_EQ(display_name("|||", "\x01", 205), std::string("Preset 205"));
}

TEST(presets, slot_file_content)
{
    const std::string text = slot_file_content(2, "Evening", {{"follow_rate_h", 6.5}, {"curve_h", 2}, {"max_lag_h", 1.0 / 3.0}});
    CHECK_EQ(text, std::string("; DWSmoothwalker Slot 2, written by the mod when you save to it from the Mod Menu.\n"
                               "; Change name below to rename the slot: the menu and the banner show it after the next game start.\n"
                               "; To make a drop-in preset from it, copy this file and give the copy any other file name.\n"
                               "name = Evening\n"
                               "follow_rate_h = 6.5\n"
                               "curve_h = 2\n"
                               "max_lag_h = 0.333333\n"));
    auto [name, values] = parse_preset_file(text);
    CHECK_EQ(name, std::string("Evening"));
    CHECK_EQ(values.size(), 3u);
    CHECK_EQ(values[2].second, 0.333333);
}

TEST(presets, with_preset_choices)
{
    const std::string manifest = "[Mod]\r\nName = DWSmoothwalker\r\n\r\n[Setting.preset]\r\nConfigKey = preset\r\n"
                                 "; PresetValues = old\r\nPresetValues = 0|101|102|103\r\nPresetLabels=Custom|Tight\r\n\r\n"
                                 "[Setting.other]\r\nPresetValues = keep\r\n";
    auto out = with_preset_choices(manifest, "[Setting.preset]", "0|101|1", "Custom|Tight|Slot 1");
    CHECK(out.has_value());
    CHECK_EQ(out.value_or(""), std::string("[Mod]\r\nName = DWSmoothwalker\r\n\r\n[Setting.preset]\r\nConfigKey = preset\r\n"
                                           "; PresetValues = old\r\nPresetValues = 0|101|1\r\nPresetLabels=Custom|Tight|Slot 1\r\n\r\n"
                                           "[Setting.other]\r\nPresetValues = keep\r\n"));
    CHECK(!with_preset_choices("[Setting.preset]\nPresetValues = 0\n", "[Setting.preset]", "0", "Custom")); // no labels line
    CHECK(!with_preset_choices("[Setting.other]\nPresetValues = 0\nPresetLabels = x\n", "[Setting.preset]", "0", "Custom"));
    CHECK(!with_preset_choices("[Setting.preset]\n; PresetValues = 0\nPresetLabels = x\n", "[Setting.preset]", "0", "Custom"));
    // No final newline stays none; an unchanged result equals the input.
    auto same = with_preset_choices("[Setting.preset]\nPresetValues = 0\nPresetLabels = Custom", "[Setting.preset]", "0", "Custom");
    CHECK_EQ(same.value_or(""), std::string("[Setting.preset]\nPresetValues = 0\nPresetLabels = Custom"));
}

TEST(presets, utf8_helpers)
{
    CHECK_EQ(dw::utf8_of(L"Café").value_or(""), std::string("Caf\xC3\xA9"));
    CHECK_EQ(dw::utf8_of(L"").value_or("x"), std::string());
    CHECK(!dw::utf8_of(std::wstring(1, static_cast<wchar_t>(0xD800)))); // a lone surrogate
    CHECK(dw::wide_of_utf8("Caf\xC3\xA9").value_or(L"") == L"Café");
    CHECK(!dw::wide_of_utf8("\xC3"));
    CHECK(dw::to_wide("a\xFF" "b") == L"a�" L"b"); // for log and banner text: replaced, not refused
    CHECK(dw::to_wide("").empty());
}

namespace
{
    struct TempDir
    {
        std::wstring path;
        TempDir()
        {
            wchar_t base[MAX_PATH]{};
            GetTempPathW(MAX_PATH, base);
            path = std::wstring(base) + L"dw_unit_presets_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
            CreateDirectoryW(path.c_str(), nullptr);
        }
        ~TempDir()
        {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    std::wstring name = data.cFileName;
                    if (name == L"." || name == L"..") continue;
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW((path + L"\\" + name).c_str());
                    else DeleteFileW((path + L"\\" + name).c_str());
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            RemoveDirectoryW(path.c_str());
        }
        auto put(const std::wstring& name, const std::string& content) const -> void
        {
            std::ofstream file(path + L"\\" + name, std::ios::binary);
            file << content;
        }
    };
} // namespace

TEST(presets, list_preset_files)
{
    TempDir dir;
    for (const wchar_t* name : {L"Slot 1.ini", L"slot 2.INI", L"Slot 7.ini", L"b.ini", L"A.ini", L"mod_settings.ini", L"notes.txt", L"Été.ini"})
        dir.put(name, "follow_rate_h = 8\n");
    CreateDirectoryW((dir.path + L"\\folder.ini").c_str(), nullptr);
    PresetFiles files = list_preset_files(dir.path);
    CHECK_EQ(files.slots.size(), 2u);
    CHECK(files.slots[1] == L"Slot 1.ini");
    CHECK(files.slots[2] == L"slot 2.INI"); // any case
    CHECK_EQ(files.dropins.size(), 4u);     // Slot 7 is past MAX_SLOTS: a drop-in
    if (files.dropins.size() == 4)
    {
        CHECK(files.dropins[0] == L"A.ini"); // case-insensitive order
        CHECK(files.dropins[1] == L"b.ini");
        CHECK(files.dropins[2] == L"Slot 7.ini");
        CHECK(files.dropins[3] == L"Été.ini");
    }
    CHECK(list_preset_files(dir.path + L"\\missing").dropins.empty());
}

TEST(presets, read_small_file)
{
    TempDir dir;
    dir.put(L"a.ini", std::string(100, 'x'));
    dir.put(L"empty.ini", "");
    CHECK_EQ(read_small_file(dir.path + L"\\a.ini", 100).value_or("").size(), 100u);
    CHECK(!read_small_file(dir.path + L"\\a.ini", 99));
    CHECK(read_small_file(dir.path + L"\\empty.ini", 10).has_value());
    CHECK(!read_small_file(dir.path + L"\\none.ini", 10));
}
