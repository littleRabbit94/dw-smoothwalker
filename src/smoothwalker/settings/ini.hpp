// smoothwalker.ini as text (docs/design.md, "The settings file"): parsing it into Settings and into the numbers a
// later change is diffed against, rewriting numbers in place as the Mod Menu does, adding the keys an older file lacks,
// and reading and writing the file.
#pragma once

#include "settings.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dw::smoothwalker::settings
{
    // ASCII whitespace off both ends.
    auto trim(std::string v) -> std::string;

    // Every key parsed onto defaults, then sanitized. A blank key name unbinds that key; a blank or non-finite number
    // keeps the default. A file without the 0.9.0 focus keys gets its combat values for them.
    auto parse_settings(const std::string& content) -> Settings;

    // The file's numeric keys as written, unsanitized: the baseline a later change is diffed against. Non-finite
    // values are left out; a repeated key keeps its last value, matching parse_settings.
    auto parse_numbers(const std::string& content) -> std::map<std::string, double>;

    // %.6g, the precision the file is written with.
    auto format_number(double v) -> std::string;

    // In place, keeping layout and comments, as the Mod Menu does.
    auto rewrite_numbers(const std::string& content, const Values& values) -> std::string;

    // The trailing comment each numeric key has in the shipped mod/config/smoothwalker.ini, for a line
    // with_missing_keys adds back. Keep in step with that file; a key without one there is left out here.
    auto shipped_comment(const std::string& key) -> const char*;

    // The Mod Menu page opens only if every ConfigKey is in the file, and rewrite_numbers changes only lines that
    // exist, so an ini copied back from an older version needs the newer keys added. A numeric key with no
    // assignment line (a commented-out one does not count; one with a bad value does, and the flush rewrites it)
    // gets "key = value ; comment" after the line of the nearest earlier NUMERIC_KEYS entry the file has, else
    // after the last line. Every other byte is kept, the newline style and a missing final newline included.
    // debug_key is added the same way, after debug_overlay: not a ConfigKey, but a blank line to fill in is how a
    // player finds it. The older key names predate this writer and are in every file it meets.
    // Returns the new content and the added keys.
    auto with_missing_keys(const std::string& content, const Settings& s) -> std::pair<std::string, std::vector<std::string>>;

    // The whole file, bytes as they are; nullopt if it cannot be opened.
    auto read_file(const std::string& path) -> std::optional<std::string>;

    // Temp file plus rename, so the Mod Menu never reads a half-written file. A failed write removes the temp file.
    auto write_file(const std::string& path, const std::string& content) -> bool;
} // namespace dw::smoothwalker::settings
