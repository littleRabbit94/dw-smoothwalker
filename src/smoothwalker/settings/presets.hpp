// Presets (docs/design.md, "Presets"): the three built-ins, slot and drop-in files in config/presets (slot files
// written by the mod, drop-ins added by hand), their parse, normalization and labels, and the Preset picker's lines in
// mod_settings.ini.
#pragma once

#include "settings.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dw::smoothwalker::settings
{
    struct NamedPreset
    {
        int id;
        const char* name;
        Values values;
    };

    // Menu ids 101-103, in cycle order, each with all PRESET_KEYS. Balanced matches the shipped defaults on
    // every key. Every value must sit inside its Mod Menu range and on its slider step.
    auto builtin_presets() -> const std::vector<NamedPreset>&;

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

    // PresetLabels is '|'-separated and must be valid UTF-8 without control characters (else the menu skips the page).
    auto clean_label(const std::string& raw) -> std::string;

    // The label: `name` cleaned, else the file stem cleaned, else "Preset <id>".
    auto display_name(const std::string& name, const std::string& stem, int id) -> std::string;

    // A preset written before 0.9.0 has no focus keys: it gets its combat values, so it loads and matches as it did.
    auto fill_focus(Values& values) -> void;

    // Values come back raw; normalize_preset clamps them.
    // Returns the name line's value and the values in file order; a repeated key keeps its first place and last value.
    auto parse_preset_file(std::string content) -> std::pair<std::string, Values>;

    // Values as they'll be live after loading and as a flush writes them: applied through the normal path (clamps,
    // rounding, flags), then round-tripped through %.6g so a loaded preset still matches its own picker entry.
    auto normalize_preset(const Values& values) -> Values;

    // name: the slot's display name, kept across saves.
    auto slot_file_content(int slot, const std::string& name, const Values& values) -> std::string;

    struct PresetFiles
    {
        std::map<int, std::wstring> slots;  // slot number -> file name
        std::vector<std::wstring> dropins;  // file names, sorted case-insensitively
    };

    // The game path and drop-in file names may be non-ASCII.
    auto list_preset_files(const std::wstring& dir) -> PresetFiles;

    // The file if it holds at most `limit` bytes; nullopt if it is larger or cannot be read.
    auto read_small_file(const std::wstring& path, size_t limit) -> std::optional<std::string>;

    // section: "[Setting.preset]". nullopt if the section or either line is missing.
    auto with_preset_choices(const std::string& manifest, const std::string& section, const std::string& values,
                             const std::string& labels) -> std::optional<std::string>;
} // namespace dw::smoothwalker::settings
