// The Mod Menu probe (ui/menu_probe.hpp). Body moved from Smoothwalker::Impl unchanged.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "menu_probe.hpp"
#include "../../common/log.hpp"

#include <cstdint>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dw::smoothwalker::ui
{
    using namespace RC;
    using namespace RC::Unreal;

    auto MenuProbe::open() -> bool
    {
        constexpr uint8_t COLLAPSED = 1; // ESlateVisibility
        auto shown = [&](UObject* object) -> bool {
            auto* visibility = object->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
            return visibility && *visibility != COLLAPSED;
        };
        if (!m_activatable_class)
        {
            m_activatable_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/CommonUI.CommonActivatableWidget"));
            if (!m_activatable_class) return false;
        }
        if (m_menu_host && !m_menu_host->IsUnreachable() && m_menu_host->GetClassPrivate() == m_activatable_class && shown(m_menu_host)) return true;
        m_menu_host = nullptr; // closed or gone; every open creates a new host
        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
            if (!object || object->GetClassPrivate() != m_activatable_class || object->IsUnreachable() || !shown(object)) return LoopAction::Continue;
            m_menu_host = object;
            return LoopAction::Break;
        });
        if (m_menu_host && !m_menu_logged)
        {
            m_menu_logged = true;
            if (dw::verbose())
                Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] Mod Menu open: {}\n"), m_menu_host->GetFullName());
        }
        return m_menu_host != nullptr;
    }
} // namespace dw::smoothwalker::ui
