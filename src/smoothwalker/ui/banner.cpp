// The on-screen banners (ui/banner.hpp). Bodies moved from Smoothwalker::Impl unchanged.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "banner.hpp"

#include <cstring>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace dw::smoothwalker::ui
{
    using namespace RC;
    using namespace RC::Unreal;

    auto Banner::request(std::wstring text) -> void
    {
        if (!m_enabled.load() || !m_core.player_known()) return;
        std::lock_guard guard(m_banner_mutex);
        m_banner_text = std::move(text);
        m_banner_due = std::chrono::steady_clock::now() + BANNER_SETTLE;
        m_banner_pending = true;
    }

    // Thins our own waiting banners so presses cannot build a backlog. The banner on screen is left alone:
    // ending a notification its widget is showing crashed the game (docs/design.md, "Rapid banners queue").
    // Ours are recognised by class and text, never by a remembered address, which the game may reuse.
    auto Banner::drop_stale_banners(UObject* subsystem) -> void
    {
        if (!subsystem) return;
        if (m_queue_offset < 0 || m_region_data_offset < 0 || !m_region_info_class)
        {
            auto* slot = subsystem->GetValuePtrByPropertyNameInChain<void>(STR("NotificationQueue"));
            m_region_info_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/DogwoodUI.RegionEnteredNotificationInfo"));
            int32_t data = -1;
            if (m_region_info_class)
            {
                for (FProperty* property : m_region_info_class->ForEachProperty())
                {
                    if (property->GetName() == STR("RegionData")) data = property->GetOffset_ForInternal();
                }
            }
            if (!slot || data < 0) return;
            m_queue_offset = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(subsystem));
            m_region_data_offset = data;
        }
        auto* queue = reinterpret_cast<ObjectArray*>(reinterpret_cast<uint8_t*>(subsystem) + m_queue_offset);
        if (queue->num < 0 || queue->num > queue->max || queue->max > 4096 || (queue->num > 0 && !queue->data)) return;

        int32_t kept = 0;
        for (int32_t i = 0; i < queue->num; ++i)
        {
            auto* entry = queue->data[i];
            if (!is_our_banner(entry)) queue->data[kept++] = entry;
        }
        queue->num = kept;
    }

    auto Banner::is_our_banner(UObject* entry) -> bool
    {
        if (!entry || entry->GetClassPrivate() != m_region_info_class) return false;
        auto* text = reinterpret_cast<FText*>(reinterpret_cast<uint8_t*>(entry) + m_region_data_offset + BANNER_TEXT);
        return text->ToString().starts_with(BANNER_PREFIX);
    }

    // The region banner shows RegionData.RegionDisplayText. The hard-coded parameter layout must match
    // reflection, or banners stay off.
    auto Banner::resolve_banner() -> bool
    {
        m_banner_state = -1;
        m_banner_function = UObjectGlobals::StaticFindObject<UFunction*>(
                nullptr, nullptr, STR("/Script/DogwoodUI.NotificationSystemLibrary:PushRegionEnteredNotification"));
        m_banner_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/DogwoodUI.Default__NotificationSystemLibrary"));
        auto* region = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/DogwoodSystem.RegionData"));
        if (!m_banner_function || !m_banner_library || !region)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] notification function not found, banners off\n"));
            return false;
        }

        auto offset_of = [](UStruct* owner, const wchar_t* name) -> int32_t {
            for (FProperty* property : owner->ForEachProperty())
            {
                if (property->GetName() == name) return property->GetOffset_ForInternal();
            }
            return -1;
        };
        auto world = offset_of(m_banner_function, STR("WorldContextObject"));
        auto data = offset_of(m_banner_function, STR("RegionData"));
        auto flag = offset_of(m_banner_function, STR("IsNewlyDiscovered"));
        auto text = offset_of(region, STR("RegionDisplayText"));
        if (world != BANNER_WORLD || data != BANNER_DATA || flag != BANNER_FLAG || text != BANNER_TEXT)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] notification layout changed ({}, {}, {}, {}), banners off\n"), world, data, flag, text);
            return false;
        }
        m_banner_state = 1;
        return true;
    }

    auto Banner::show_pending() -> void
    {
        auto* controller = m_core.player_controller(); // checked live this tick
        if (!controller) return;
        std::wstring line;
        {
            std::lock_guard guard(m_banner_mutex);
            if (!m_banner_pending || std::chrono::steady_clock::now() < m_banner_due) return;
            m_banner_pending = false;
            line = m_banner_text;
        }
        if (m_banner_state == 0) resolve_banner();
        if (m_banner_state != 1) return;

        // Cached: FindFirstOf walks the whole object array (28 ms measured), and this runs mid-glide after a preset change.
        if (!m_notifications.alive()) m_notifications = dw::LiveRef::of(UObjectGlobals::FindFirstOf(STR("NotificationSubsystem")));
        drop_stale_banners(m_notifications.object);
        FText text(line.c_str());
        uint8_t params[BANNER_PARAMS_SIZE]{};
        memcpy(params + BANNER_WORLD, &controller, sizeof(controller));
        text.CopyBorrowedTo(params + BANNER_DATA + BANNER_TEXT);
        params[BANNER_FLAG] = 0; // not newly discovered: no discovery reward
        m_banner_library->ProcessEvent(m_banner_function, params);
    }
} // namespace dw::smoothwalker::ui
