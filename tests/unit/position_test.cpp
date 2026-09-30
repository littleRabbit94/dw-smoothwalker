// smoothwalker/modes/position: the camera position the settings make (position_of) over the defaults and each built-in
// preset, the switches, equality, the tuned mode classes, one offset entry as a publish writes it (offset_of, with
// the interior settings), and the rule for what a publish writes (written).

#include "check.hpp"
#include "smoothwalker/modes/position.hpp"
#include "smoothwalker/settings/presets.hpp"
#include "smoothwalker/settings/settings.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>

using namespace dw::smoothwalker;
using modes::GroupTuning;
using modes::InteriorTuning;
using modes::OffsetValues;
using modes::PositionTuning;
using modes::offset_of;
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
    CHECK(p.interior == InteriorTuning{});
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
        CHECK_EQ(p.interior.distance, value_of(preset.values, "interior_distance"));
        CHECK_EQ(p.interior.height, value_of(preset.values, "interior_height"));
        CHECK_EQ(p.interior.shoulder, value_of(preset.values, "interior_shoulder"));
        CHECK_EQ(p.interior.fov, value_of(preset.values, "interior_fov"));
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
    CHECK(balanced.interior == InteriorTuning{});
    CHECK(tight.interior == InteriorTuning{}); // the built-ins leave the indoor camera to the groups

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
    s.interior_distance = 80;
    s.interior_height = -5;
    s.interior_shoulder = 15;
    s.interior_fov = -10;
    p = position_of(s);
    CHECK(p.active);
    CHECK(p.own_lag);
    CHECK(p.shoulder_swap);
    CHECK_EQ(p.transition, 1.25);
    CHECK_EQ(p.pitch_min, -45.0);
    CHECK_EQ(p.pitch_max, 70.0);
    CHECK_EQ(p.groups[modes::Traversal].fov, -7.0);
    CHECK_EQ(p.interior.distance, 80.0);
    CHECK_EQ(p.interior.height, -5.0);
    CHECK_EQ(p.interior.shoulder, 15.0);
    CHECK_EQ(p.interior.fov, -10.0);
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
    p.interior.distance = 99;
    CHECK(!(p == base));
    p = base;
    p.interior.height = 1;
    CHECK(!(p == base));
    p = base;
    p.interior.shoulder = 1;
    CHECK(!(p == base));
    p = base;
    p.interior.fov = 1;
    CHECK(!(p == base));
    p = base;
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
    s.interior_fov = -10;
    const PositionTuning active = position_of(s);
    CHECK(active.active);
    CHECK(written(active) == active); // the interior settings included
}

TEST(position, written_of_an_inactive_position_is_the_lag_switch_only)
{
    Settings s;
    s.camera_tuning = false;
    s.exploration_distance = 120;
    s.sprint_fov = 9;
    s.shoulder_swap = true;
    s.pitch_max = 60;
    s.interior_distance = 120;
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

namespace
{
    // A mode's key with a shoulder, as offset_of() gets it from the base.
    auto base_of(uint8_t key) -> OffsetValues
    {
        return {key, -190.0, 70.0, 10.0, 0.0f, false};
    }

    auto same_offset(const OffsetValues& a, const OffsetValues& b) -> bool
    {
        return a.key == b.key && a.x == b.x && a.y == b.y && a.z == b.z && a.overridden_fov == b.overridden_fov &&
               a.override_fov == b.override_fov;
    }
} // namespace

TEST(offset_of, neutral_tuning_changes_nothing_on_any_key)
{
    for (uint8_t key : {uint8_t{1}, uint8_t{2}, uint8_t{3}})
    {
        for (bool interior : {false, true})
        {
            const OffsetValues b = base_of(key);
            CHECK(same_offset(offset_of(b, 90.0f, GroupTuning{}, InteriorTuning{}, interior, false), b));
            OffsetValues centred = b;
            centred.x = 0.0;
            centred.y = 0.0;
            centred.z = -4.5;
            centred.overridden_fov = 70.0f;
            centred.override_fov = true;
            CHECK(same_offset(offset_of(centred, 90.0f, GroupTuning{}, InteriorTuning{}, interior, false), centred));
        }
    }
}

TEST(offset_of, neutral_interior_is_the_group_alone_on_key_2)
{
    // Bit for bit: key 2 of a mode that takes the interior settings, at neutral, writes what a mode that does not
    // take them writes, for a group and a swap that move every field.
    const GroupTuning g{85, -5, 15, 7};
    OffsetValues b = base_of(2);
    b.x = -190.3;
    b.overridden_fov = 160.0f;
    for (bool swap : {false, true})
    {
        const OffsetValues with = offset_of(b, 97.0f, g, InteriorTuning{}, true, swap);
        const OffsetValues without = offset_of(b, 97.0f, g, InteriorTuning{}, false, swap);
        CHECK(std::memcmp(&with.x, &without.x, sizeof(double)) == 0);
        CHECK(std::memcmp(&with.y, &without.y, sizeof(double)) == 0);
        CHECK(std::memcmp(&with.z, &without.z, sizeof(double)) == 0);
        CHECK(std::memcmp(&with.overridden_fov, &without.overridden_fov, sizeof(float)) == 0);
        CHECK_EQ(with.override_fov, without.override_fov);
    }
}

TEST(offset_of, only_key_2_of_a_mode_that_takes_it_sees_the_interior_settings)
{
    const InteriorTuning in{120, 5, 10, -10};
    for (uint8_t key : {uint8_t{1}, uint8_t{3}})
    {
        const OffsetValues b = base_of(key);
        CHECK(same_offset(offset_of(b, 90.0f, GroupTuning{}, in, true, false), b));
    }
    const OffsetValues k2 = base_of(2);
    CHECK(same_offset(offset_of(k2, 90.0f, GroupTuning{}, in, false, false), k2)); // a mode that does not take them
    CHECK(!same_offset(offset_of(k2, 90.0f, GroupTuning{}, in, true, false), k2));
}

TEST(offset_of, interior_settings_stack_on_the_groups)
{
    const GroupTuning g{75, 5, 25, 0};
    const InteriorTuning in{120, 5, 10, 0};
    const OffsetValues o = offset_of(base_of(2), 90.0f, g, in, true, false);
    CHECK_NEAR(o.x, -171.0, 1e-9); // -190 * 0.75 * 1.2
    CHECK_NEAR(o.y, 105.0, 1e-9);  // 70 + 25 + 10
    CHECK_NEAR(o.z, 20.0, 1e-9);   // 10 + 5 + 5
    // The other keys get the group's part only.
    const OffsetValues d = offset_of(base_of(1), 90.0f, g, in, true, false);
    CHECK_NEAR(d.x, -142.5, 1e-9);
    CHECK_NEAR(d.y, 95.0, 1e-9);
    CHECK_NEAR(d.z, 15.0, 1e-9);
    // Only a base behind the character moves with the distance.
    OffsetValues front = base_of(2);
    front.x = 30.0;
    CHECK_EQ(offset_of(front, 90.0f, g, in, true, false).x, 30.0);
}

TEST(offset_of, combined_shoulder_stops_at_the_centre)
{
    OffsetValues b = base_of(2);
    b.y = 30.0;
    const GroupTuning g{100, 0, -20, 0};
    const InteriorTuning in{100, 0, -20, 0};
    CHECK_EQ(offset_of(b, 90.0f, g, in, true, false).y, 0.0); // 30 - 20 - 20 does not cross to the other side
    b.y = -30.0;
    CHECK_EQ(offset_of(b, 90.0f, g, in, true, false).y, 0.0);
    CHECK_NEAR(offset_of(b, 90.0f, GroupTuning{}, InteriorTuning{100, 0, 10, 0}, true, false).y, -40.0, 1e-9); // grows away from the centre

    // A centred base stays centred whatever the shoulder settings.
    b.y = 0.0;
    CHECK_EQ(offset_of(b, 90.0f, GroupTuning{100, 0, 25, 0}, InteriorTuning{100, 0, 30, 0}, true, false).y, 0.0);
    CHECK_EQ(offset_of(b, 90.0f, GroupTuning{100, 0, 25, 0}, InteriorTuning{100, 0, 30, 0}, true, true).y, 0.0);

    // The swap negates after the tuning.
    b.y = 70.0;
    CHECK_NEAR(offset_of(b, 90.0f, GroupTuning{100, 0, 25, 0}, InteriorTuning{100, 0, 10, 0}, true, true).y, -105.0, 1e-9);
    CHECK_NEAR(offset_of(b, 90.0f, GroupTuning{100, 0, 25, 0}, InteriorTuning{100, 0, 10, 0}, true, false).y, 105.0, 1e-9);
}

TEST(offset_of, interior_fov_switches_the_override_on_for_key_2)
{
    // No interior FOV: the override stays as the base has it, with the group's FOV added to the value as before.
    OffsetValues o = offset_of(base_of(2), 95.0f, GroupTuning{100, 0, 0, 5}, InteriorTuning{}, true, false);
    CHECK(!o.override_fov);
    CHECK_EQ(o.overridden_fov, 5.0f);

    // From the mode's FOV as written (base 90 plus the group's 5).
    o = offset_of(base_of(2), 95.0f, GroupTuning{100, 0, 0, 5}, InteriorTuning{100, 0, 0, -10}, true, false);
    CHECK(o.override_fov);
    CHECK_EQ(o.overridden_fov, 85.0f);
    o = offset_of(base_of(2), 90.0f, GroupTuning{}, InteriorTuning{100, 0, 0, -10}, true, false);
    CHECK(o.override_fov);
    CHECK_EQ(o.overridden_fov, 80.0f);

    // A base that already overrides is the starting point instead of the mode's FOV.
    OffsetValues overriding = base_of(2);
    overriding.override_fov = true;
    overriding.overridden_fov = 70.0f;
    o = offset_of(overriding, 90.0f, GroupTuning{100, 0, 0, 5}, InteriorTuning{100, 0, 0, -10}, true, false);
    CHECK(o.override_fov);
    CHECK_EQ(o.overridden_fov, 65.0f);

    // Other keys, and modes that do not take the settings, get no override.
    o = offset_of(base_of(1), 90.0f, GroupTuning{}, InteriorTuning{100, 0, 0, -10}, true, false);
    CHECK(!o.override_fov);
    o = offset_of(base_of(2), 90.0f, GroupTuning{}, InteriorTuning{100, 0, 0, -10}, false, false);
    CHECK(!o.override_fov);
}

TEST(position, interior_modes)
{
    const std::set<std::wstring> expected{L"Base", L"Base_LongRange", L"Base_CloseRange", L"Base_CloseRange_Mantle2m", L"Sprint",
                                          L"Sprint_VampiricFastTraversal", L"FocusMode", L"AntiGrav", L"Shadowstep_2_Base"};
    std::set<std::wstring> found;
    for (const auto& spec : modes::mode_classes())
    {
        if (spec.interior) found.insert(spec.name);
    }
    CHECK(found == expected);
}

TEST(position, shown_fov_per_camera_type)
{
    // Base_LongRange's keys as shipped (docs/design.md, "Indoors"): the override off, its value unused.
    const std::vector<OffsetValues> keys{{1, -250, 30, 0, 160.0f, false}, {2, -190, 70, 10, 160.0f, false}, {3, -250, 70, 0, 160.0f, false}};
    const modes::ModeClassSpec spec{modes::Exploration, L"Base_LongRange", L"", true, true};
    PositionTuning t;
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 1), 90.0f);
    t.groups[modes::Exploration].fov = 5;
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 1), 95.0f);
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 2), 95.0f); // no interior FOV: the same indoors
    t.interior.fov = -10;
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 2), 85.0f); // the override the write switches on
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 1), 95.0f);
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 3), 95.0f);
    // A mode that does not take the interior settings keeps its FOV indoors.
    const modes::ModeClassSpec plain{modes::Exploration, L"GapSqueeze", L"", true, false};
    CHECK_EQ(modes::shown_fov(90.0f, keys, plain, t, 2), 95.0f);
    // A type without a key falls back to Default's; no keys at all, the mode's FOV.
    const std::vector<OffsetValues> default_only{{1, -250, 30, 0, 70.0f, true}}; // another mod's override on key 1
    CHECK_EQ(modes::shown_fov(90.0f, default_only, spec, t, 2), 75.0f);
    CHECK_EQ(modes::shown_fov(90.0f, {}, spec, t, 2), 95.0f);
    // Tuning off: the base as it stands.
    t.active = false;
    CHECK_EQ(modes::shown_fov(90.0f, keys, spec, t, 2), 90.0f);
    CHECK_EQ(modes::shown_fov(90.0f, default_only, spec, t, 1), 70.0f);
}
