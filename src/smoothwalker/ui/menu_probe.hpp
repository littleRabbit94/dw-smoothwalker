// Whether a Dawnwalker Mod Menu page may be open (docs/design.md, "The settings file"): the check that keeps the settings write-back
// from landing under an open page while the camera is paused. Game thread only.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>

namespace dw::smoothwalker::ui
{
    class MenuProbe
    {
      public:
        // The Dawnwalker Mod Menu (Nexus 271) creates its host as a plain CommonActivatableWidget (main.lua, library:Create
        // with the native class, which the game's own screens all subclass), enabled and shown while the menu is open and
        // Collapsed once closed, when it lingers until GC. Any such widget not Collapsed means a settings page may be open.
        // Only reached with a write pending and the camera not live. A menu that changes its host is not recognised, and
        // the write then lands under the open page as it did before 0.9: the page refuses its next Apply until reopened.
        auto open() -> bool;

      private:
        RC::Unreal::UClass* m_activatable_class = nullptr; // CommonActivatableWidget, the Mod Menu's host class
        RC::Unreal::UObject* m_menu_host = nullptr;        // the Mod Menu host last seen open; checked before any rescan
        bool m_menu_logged = false;
    };
} // namespace dw::smoothwalker::ui
