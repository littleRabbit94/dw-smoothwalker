// smoothwalker/ui/panel: golden strings for format_panel over representative panels (following, idle with NaNs, the
// camera API's three states, a glide, the mode list's overflow), and the name of every snap reason and influence.

#include "check.hpp"
#include "smoothwalker/ui/panel.hpp"

#include <cmath>
#include <string>

using namespace dw::smoothwalker;
using dw::camera::Snap;
using ui::ApiStatus;
using ui::DebugPanel;
using ui::format_panel;

namespace
{
    // A panel mid-follow: three modes, a combat influence, a teleport snap, no API claim.
    auto following() -> DebugPanel
    {
        DebugPanel p;
        p.enabled = true;
        p.preset = L"Balanced";
        p.camera_tuning = true;
        p.camera_type = L"Default";
        p.modes = {{L"Base", 1, modes::Exploration, true},
                   {L"Aiming", 0, modes::Aiming, true},
                   {L"ShadowstepAttack", 2, -1, false}};
        p.keep_follow = 0.75;
        p.keep_turn = 0.4;
        p.rotation_smoothing = true;
        p.influence = follow::Influence::Combat;
        p.lag_h = 12.4;
        p.lag_v = 3.6;
        p.max_lag_h = 60.0;
        p.max_lag_v = 30.0;
        p.rate_h = 4.26;
        p.snap = Snap::Teleport;
        p.snap_age = 12.4;
        return p;
    }
} // namespace

TEST(panel, following)
{
    CHECK(format_panel(following()) ==
          L"Smoothwalker  ON   preset: Balanced   tuning: on\n"
          L"camera type   Default\n"
          L"modes         Base                active     [Exploration, tuned]\n"
          L"              Aiming              blend in   [Aiming, tuned]\n"
          L"              ShadowstepAttack    blend out  [untracked]\n"
          L"follow 75%   turn 40%   (combat)\n"
          L"lag h 12/60 cm   v 4/30 cm   rate 4.3/s\n"
          L"last snap     teleport, 12 s ago\n"
          L"api           none");
}

TEST(panel, idle_with_nans)
{
    DebugPanel p; // the defaults: off, nothing known
    p.preset = L"Custom";
    p.modes_stale = true;
    CHECK(format_panel(p) ==
          L"Smoothwalker  OFF  preset: Custom   tuning: off\n"
          L"camera type   ?\n"
          L"modes         none\n"
          L"              (rescan pending)\n"
          L"follow -   turn off\n"
          L"lag h -/0 cm   v -/0 cm   rate -\n"
          L"last snap     none yet\n"
          L"api           none");
}

TEST(panel, turn_smoothing_on_without_a_value)
{
    DebugPanel p = following();
    p.keep_turn = NAN;
    CHECK(format_panel(p).contains(L"follow 75%   turn -   (combat)\n"));
    p.keep_follow = NAN; // no follow: no influence in parentheses
    CHECK(format_panel(p).contains(L"follow -   turn -\nlag h"));
}

TEST(panel, api_owner_with_lease)
{
    DebugPanel p = following();
    p.api.owner = L"DWFreeCam";
    p.api.lease = 3.26;
    CHECK(format_panel(p).ends_with(L"last snap     teleport, 12 s ago\napi           DWFreeCam lease 3.3 s"));
}

TEST(panel, api_owner_without_lease)
{
    DebugPanel p = following();
    p.api.owner = L"DWFreeCam";
    CHECK(format_panel(p).ends_with(L"api           DWFreeCam (no lease)"));
}

TEST(panel, api_glide_and_owner_over_glide)
{
    DebugPanel p = following();
    p.api.glide = true;
    CHECK(format_panel(p).ends_with(L"api           glide"));
    p.api.owner = L"DWFreeCam"; // a claim shows as the owner even while a crossfade runs
    CHECK(format_panel(p).ends_with(L"api           DWFreeCam (no lease)"));
}

TEST(panel, snap_reasons_by_name)
{
    struct Case
    {
        Snap snap;
        const wchar_t* name;
    };
    const Case cases[]{
            {Snap::Startup, L"startup"},          {Snap::Teleport, L"teleport"},
            {Snap::Gap, L"gap"},                  {Snap::World, L"world change"},
            {Snap::Player, L"player change"},     {Snap::Pawn, L"pawn change"},
            {Snap::Toggle, L"toggle"},            {Snap::ApiCut, L"api cut"},
            {Snap::ClaimEnded, L"api lease ended"}, {Snap::ViewLost, L"view lost"},
    };
    for (const Case& c : cases)
    {
        DebugPanel p;
        p.snap = c.snap;
        p.snap_age = 5.0;
        CHECK(std::wstring(ui::snap_name(c.snap)) == c.name);
        CHECK(format_panel(p).contains(std::wstring(L"last snap     ") + c.name + L", 5 s ago\n"));
    }
    CHECK(std::wstring(ui::snap_name(Snap::None)) == L"none");
    CHECK(std::wstring(ui::snap_name(static_cast<Snap>(99))) == L"none");
}

TEST(panel, no_snap_yet_when_none_or_age_unknown)
{
    DebugPanel p;
    p.snap = Snap::World;
    p.snap_age = NAN;
    CHECK(format_panel(p).contains(L"last snap     none yet\n"));
    p.snap = Snap::None;
    p.snap_age = 5.0;
    CHECK(format_panel(p).contains(L"last snap     none yet\n"));
}

TEST(panel, influence_names)
{
    CHECK(std::wstring(ui::influence_name(follow::Influence::None)) == L"none");
    CHECK(std::wstring(ui::influence_name(follow::Influence::Traversal)) == L"traversal");
    CHECK(std::wstring(ui::influence_name(follow::Influence::Combat)) == L"combat");
    CHECK(std::wstring(ui::influence_name(follow::Influence::Aiming)) == L"aiming");
    DebugPanel p = following();
    p.influence = follow::Influence::Aiming;
    CHECK(format_panel(p).contains(L"follow 75%   turn 40%   (aiming)\n"));
}

TEST(panel, mode_list_shows_eight_and_counts_the_rest)
{
    DebugPanel p;
    for (int i = 0; i < 11; ++i) p.modes.push_back({L"M" + std::to_wstring(i), 1, modes::Sprint, false});
    const std::wstring text = format_panel(p);
    CHECK(text.contains(L"modes         M0                  active     [Sprint, as shipped]\n"));
    CHECK(text.contains(L"              M7                  active     [Sprint, as shipped]\n"));
    CHECK(!text.contains(L"M8"));
    CHECK(text.contains(L"              +3 more\n"));
}

TEST(panel, every_group_has_a_name)
{
    for (int g = 0; g < modes::GroupCount; ++g) CHECK(std::wstring(modes::group_name(static_cast<modes::Group>(g))) != L"?");
    CHECK(std::wstring(modes::group_name(modes::GroupCount)) == L"?");
    CHECK(std::wstring(modes::group_name(static_cast<modes::Group>(-1))) == L"?");
}
