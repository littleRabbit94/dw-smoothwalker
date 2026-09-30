// The debug overlay's text: what the panel shows (DebugPanel, ApiStatus) and its layout (format_panel), as pure code so
// the unit tests compile it. The widget that shows it is ui/debug_overlay.hpp; the game-thread gathering is
// smoothwalker.cpp, debug_panel (docs/design.md, "Debug overlay").
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "../modes/position.hpp"
#include "../follow/follow.hpp"
#include "../../camera/frame.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace dw::smoothwalker::ui
{
    using modes::Group;
    using camera::Snap;
    using follow::Influence;
    using modes::group_name;
    using modes::ModeListing;

    // The camera API as the panel shows it.
    struct ApiStatus
    {
        std::wstring owner; // empty: nobody holds the camera
        double lease = NAN; // s left on the owner's lease; NAN: no lease
        bool glide = false; // the hook is crossfading back after a release("glide")
    };

    // What the panel shows, gathered on the game thread at a refresh. NAN: not known (the follow is off, or has not
    // run on this camera yet).
    struct DebugPanel
    {
        bool enabled = false;
        std::wstring preset;
        bool camera_tuning = false;
        std::wstring camera_type;
        std::vector<ModeListing> modes; // newest first
        bool modes_stale = false;       // a rescan is pending, the list may be short
        double keep_follow = NAN;       // share of the trail shown, after the interior, traversal, focus, combat and aiming blends
        double keep_turn = NAN;         // share of the turning smoothing shown
        bool rotation_smoothing = false;
        Influence influence = Influence::None;
        double lag_h = NAN, lag_v = NAN; // cm of lag on screen
        double max_lag_h = 0.0, max_lag_v = 0.0;
        double rate_h = NAN; // 1/s, the horizontal follow rate after the curve
        Snap snap = Snap::None;
        double snap_age = NAN; // s since that snap
        ApiStatus api;
    };

    inline auto snap_name(Snap snap) -> const wchar_t*
    {
        switch (snap)
        {
        case Snap::Startup: return L"startup";
        case Snap::Teleport: return L"teleport";
        case Snap::Gap: return L"gap";
        case Snap::World: return L"world change";
        case Snap::Player: return L"player change";
        case Snap::Pawn: return L"pawn change";
        case Snap::Toggle: return L"toggle";
        case Snap::ApiCut: return L"api cut";
        case Snap::ClaimEnded: return L"api lease ended";
        case Snap::ViewLost: return L"view lost";
        default: return L"none";
        }
    }

    inline auto influence_name(Influence influence) -> const wchar_t*
    {
        switch (influence)
        {
        case Influence::Interior: return L"interior";
        case Influence::Traversal: return L"traversal";
        case Influence::Focus: return L"focus";
        case Influence::Combat: return L"combat";
        case Influence::Aiming: return L"aiming";
        default: return L"none";
        }
    }

    // One value per line. Roboto is proportional, so the padding lines columns up only roughly.
    inline auto format_panel(const DebugPanel& p) -> std::wstring
    {
        constexpr size_t MAX_MODES = 8;
        constexpr const wchar_t* INDENT = L"              ";
        auto number = [](double v, const wchar_t* unit) -> std::wstring {
            return std::isfinite(v) ? std::format(L"{:.0f}{}", v, unit) : std::wstring(L"-");
        };

        std::wstring out = std::format(L"Smoothwalker  {:<5}preset: {}   tuning: {}\n", p.enabled ? L"ON" : L"OFF", p.preset,
                                       p.camera_tuning ? L"on" : L"off");
        out += std::format(L"camera type   {}\n", p.camera_type.empty() ? std::wstring(L"?") : p.camera_type);

        if (p.modes.empty()) out += L"modes         none\n";
        for (size_t i = 0; i < p.modes.size() && i < MAX_MODES; ++i)
        {
            const auto& m = p.modes[i];
            const wchar_t* state = m.state == 0 ? L"blend in" : m.state == 1 ? L"active" : L"blend out";
            auto tag = m.group < 0 ? std::wstring(L"[untracked]")
                                   : std::format(L"[{}, {}]", group_name(static_cast<Group>(m.group)), m.tuned ? L"tuned" : L"as shipped");
            out += std::format(L"{}{:<20}{:<11}{}\n", i == 0 ? L"modes         " : INDENT, m.name, state, tag);
        }
        if (p.modes.size() > MAX_MODES) out += std::format(L"{}+{} more\n", INDENT, p.modes.size() - MAX_MODES);
        if (p.modes_stale) out += std::format(L"{}(rescan pending)\n", INDENT);

        auto turn = !p.rotation_smoothing ? std::wstring(L"off") : number(p.keep_turn * 100.0, L"%");
        out += std::format(L"follow {}   turn {}", number(p.keep_follow * 100.0, L"%"), turn);
        out += std::isfinite(p.keep_follow) ? std::format(L"   ({})\n", influence_name(p.influence)) : std::wstring(L"\n");
        out += std::format(L"lag h {}/{:.0f} cm   v {}/{:.0f} cm   rate {}\n", number(p.lag_h, L""), p.max_lag_h, number(p.lag_v, L""), p.max_lag_v,
                           std::isfinite(p.rate_h) ? std::format(L"{:.1f}/s", p.rate_h) : std::wstring(L"-"));

        if (p.snap == Snap::None || !std::isfinite(p.snap_age)) out += L"last snap     none yet\n";
        else out += std::format(L"last snap     {}, {:.0f} s ago\n", snap_name(p.snap), p.snap_age);

        if (!p.api.owner.empty())
        {
            out += std::isfinite(p.api.lease) ? std::format(L"api           {} lease {:.1f} s", p.api.owner, p.api.lease)
                                              : std::format(L"api           {} (no lease)", p.api.owner);
        }
        else out += p.api.glide ? L"api           glide" : L"api           none";
        return out;
    }
} // namespace dw::smoothwalker::ui
