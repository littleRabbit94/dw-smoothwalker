// smoothwalker/modes/position: the camera position the settings make (position_of) over the defaults and each built-in
// preset, the switches, equality, the tuned mode classes, and the rule for what a publish writes (written).

#include "check.hpp"
#include "smoothwalker/modes/position.hpp"
#include "smoothwalker/settings/presets.hpp"
#include "smoothwalker/settings/settings.hpp"

#include <algorithm>
#include <set>
#include <string>

using namespace dw::smoothwalker;
using modes::PositionTuning;
using modes::position_of;
using modes::written;
using settings::Settings;

namespace
{
    auto value_of(const settings::Values& values, const std::string& key) -> double
    {
        auto it = std::find_if(values.begin(), values.end(), [&](auto& e) { return e.first == key; });
        return it == values.end() ? -1e9 : it->second;
    }

    auto settings_of(const settings::NamedPreset& preset) -> Settings
    {
        Settings s;
        settings::apply_values(s, preset.values);
        return s;
    }

    auto group_is(const PositionTuning& p, modes::Group g, double distance, double height, double shoulder, double fov) -> bool
    {
        const auto& t = p.groups[g];
        return t.distance == distance && t.height == height && t.shoulder == shoulder && t.fov == fov;
    }
} // namespace

TEST(position, defaults_leave_the_modes_as_shipped)
{
    const PositionTuning p = position_of(Settings{});
    CHECK(p.active);
    CHECK(p.own_lag);
    for (int g = 0; g < modes::GroupCount; ++g) CHECK(group_is(p, static_cast<modes::Group>(g), 100, 0, 0, 0));
    CHECK(!p.shoulder_swap);
    CHECK_EQ(p.pitch_min, -60.0);
    CHECK_EQ(p.pitch_max, 40.0);
    CHECK_EQ(p.transition, 0.5);
    CHECK(p == PositionTuning{}); // a default PositionTuning is the shipped position too
}

TEST(position, every_builtin_preset_maps_key_by_key)
{
    const char* names[modes::GroupCount]{"exploration", "sprint", "combat", "focus", "aiming", "traversal"};
    for (const auto& preset : settings::builtin_presets())
    {
        const PositionTuning p = position_of(settings_of(preset));
        for (int g = 0; g < modes::GroupCount; ++g)
        {
            const std::string n = names[g];
            CHECK_EQ(p.groups[g].distance, value_of(preset.values, n + "_distance"));
            CHECK_EQ(p.groups[g].height, value_of(preset.values, n + "_height"));
            CHECK_EQ(p.groups[g].fov, value_of(preset.values, n + "_fov"));
            // The traversal group has no shoulder setting: its offset stays 0.
            CHECK_EQ(p.groups[g].shoulder, g == modes::Traversal ? 0.0 : value_of(preset.values, n + "_shoulder"));
        }
        CHECK_EQ(p.pitch_min, value_of(preset.values, "pitch_min"));
        CHECK_EQ(p.pitch_max, value_of(preset.values, "pitch_max"));
        // A preset carries no switch: the position stays live and keeps the default transition and shoulder.
        CHECK(p.active);
        CHECK(p.own_lag);
        CHECK(!p.shoulder_swap);
        CHECK_EQ(p.transition, 0.5);
    }
}

TEST(position, tight_balanced_and_cinematic_values)
{
    const auto& b = settings::builtin_presets();
    const PositionTuning tight = position_of(settings_of(b[0]));
    const PositionTuning balanced = position_of(settings_of(b[1]));
    const PositionTuning cinematic = position_of(settings_of(b[2]));

    CHECK(group_is(tight, modes::Exploration, 90, 0, 0, 0));
    CHECK(group_is(tight, modes::Sprint, 90, 0, 0, 0));
    CHECK(group_is(tight, modes::Combat, 95, 0, 0, 0));
    CHECK(group_is(tight, modes::Focus, 95, 0, 0, 0));
    CHECK(group_is(tight, modes::Aiming, 100, 0, 0, 0));
    CHECK(group_is(tight, modes::Traversal, 95, 0, 0, 0));

    CHECK(balanced == position_of(Settings{})); // Balanced is the shipped defaults

    CHECK(group_is(cinematic, modes::Exploration, 115, 10, 10, 5));
    CHECK(group_is(cinematic, modes::Sprint, 110, 10, 10, 8));
    CHECK(group_is(cinematic, modes::Combat, 110, 0, 0, 0));
    CHECK(group_is(cinematic, modes::Focus, 110, 0, 0, 0));
    CHECK(group_is(cinematic, modes::Aiming, 100, 0, 0, 0));
    CHECK(group_is(cinematic, modes::Traversal, 115, 0, 0, 5));
    CHECK_EQ(cinematic.pitch_min, -70.0);
    CHECK_EQ(cinematic.pitch_max, 55.0);

    CHECK(!(tight == balanced));
    CHECK(!(cinematic == balanced));
    CHECK(!(tight == cinematic));
}

TEST(position, switches_and_pass_through_values)
{
    Settings s;
    s.enabled = false; // off is the game as shipped: modes and its own lag
    PositionTuning p = position_of(s);
    CHECK(!p.active);
    CHECK(!p.own_lag);

    s.enabled = true;
    s.camera_tuning = false; // the follow runs, the modes are not tuned
    p = position_of(s);
    CHECK(!p.active);
    CHECK(p.own_lag);

    s.camera_tuning = true;
    s.shoulder_swap = true;
    s.position_transition = 1.25;
    s.pitch_min = -45;
    s.pitch_max = 70;
    s.traversal_fov = -7;
    p = position_of(s);
    CHECK(p.active);
    CHECK(p.own_lag);
    CHECK(p.shoulder_swap);
    CHECK_EQ(p.transition, 1.25);
    CHECK_EQ(p.pitch_min, -45.0);
    CHECK_EQ(p.pitch_max, 70.0);
    CHECK_EQ(p.groups[modes::Traversal].fov, -7.0);
}

TEST(position, equality_sees_every_field)
{
    const PositionTuning base = position_of(Settings{});
    CHECK(base == position_of(Settings{}));

    for (int g = 0; g < modes::GroupCount; ++g)
    {
        PositionTuning p = base;
        p.groups[g].distance = 99;
        CHECK(!(p == base));
        p = base;
        p.groups[g].height = 1;
        CHECK(!(p == base));
        p = base;
        p.groups[g].shoulder = 1;
        CHECK(!(p == base));
        p = base;
        p.groups[g].fov = 1;
        CHECK(!(p == base));
    }
    PositionTuning p = base;
    p.active = false;
    CHECK(!(p == base));
    p = base;
    p.own_lag = false;
    CHECK(!(p == base));
    p = base;
    p.shoulder_swap = true;
    CHECK(!(p == base));
    p = base;
    p.pitch_min = -61;
    CHECK(!(p == base));
    p = base;
    p.pitch_max = 41;
    CHECK(!(p == base));
    p = base;
    p.transition = 0.6;
    CHECK(!(p == base));
}

TEST(position, written_keeps_an_active_position_whole)
{
    Settings s;
    s.exploration_distance = 120;
    s.shoulder_swap = true;
    const PositionTuning active = position_of(s);
    CHECK(active.active);
    CHECK(written(active) == active);
}

TEST(position, written_of_an_inactive_position_is_the_lag_switch_only)
{
    Settings s;
    s.camera_tuning = false;
    s.exploration_distance = 120;
    s.sprint_fov = 9;
    s.shoulder_swap = true;
    s.pitch_max = 60;
    const PositionTuning w = written(position_of(s));
    CHECK(!w.active);
    CHECK(w.own_lag); // enabled: the game's lag stays off
    // Everything else is the default position, whatever the settings hold.
    PositionTuning lag_only;
    lag_only.active = false;
    CHECK(w == lag_only);

    s.enabled = false;
    CHECK(!written(position_of(s)).own_lag);
}

TEST(position, a_changed_number_with_tuning_off_is_no_change)
{
    Settings a;
    a.camera_tuning = false;
    Settings b = a;
    b.exploration_distance = 130;
    b.aiming_height = 20;
    b.position_transition = 1.0;
    CHECK(!(position_of(a) == position_of(b)));
    CHECK(written(position_of(a)) == written(position_of(b))); // nothing to apply

    b.camera_tuning = true; // switching it on is a change, and applies the latest values
    CHECK(!(written(position_of(a)) == written(position_of(b))));
}

TEST(position, written_tells_the_lag_switch_and_the_tuning_switch_apart)
{
    Settings on;
    Settings tuning_off = on;
    tuning_off.camera_tuning = false;
    Settings off = on;
    off.enabled = false;
    CHECK(!(written(position_of(on)) == written(position_of(tuning_off))));
    CHECK(!(written(position_of(tuning_off)) == written(position_of(off)))); // own_lag differs
    CHECK(!(written(position_of(on)) == written(position_of(off))));
}

TEST(position, tuned_mode_classes)
{
    const auto& specs = modes::mode_classes();
    CHECK_EQ(specs.size(), 23u);

    int per_group[modes::GroupCount]{};
    int player_pitch = 0;
    std::set<std::wstring> names;
    for (const auto& spec : specs)
    {
        ++per_group[spec.group];
        if (spec.player_pitch) ++player_pitch;
        names.insert(spec.name);
        // Only the shadowstep camera sits in a folder under Modes/, and the folder carries its slash.
        const bool shadowstep = std::wstring(spec.name) == L"Shadowstep_2_Base";
        CHECK(std::wstring(spec.folder) == (shadowstep ? L"Shadowstep/" : L""));
    }
    CHECK_EQ(names.size(), specs.size());
    CHECK_EQ(per_group[modes::Exploration], 5);
    CHECK_EQ(per_group[modes::Sprint], 2);
    CHECK_EQ(per_group[modes::Combat], 6);
    CHECK_EQ(per_group[modes::Focus], 1);
    CHECK_EQ(per_group[modes::Aiming], 5);
    CHECK_EQ(per_group[modes::Traversal], 4);
    // The look limits apply to the modes shipped at -60 / 40: every exploration and sprint mode, focus, shadowstep.
    CHECK_EQ(player_pitch, 9);
    for (const auto& spec : specs)
    {
        const bool takes = spec.group == modes::Exploration || spec.group == modes::Sprint || spec.group == modes::Focus ||
                           std::wstring(spec.name) == L"Shadowstep_2_Base";
        CHECK_EQ(spec.player_pitch, takes);
    }
}
