// The on-screen banners ("Smoothwalker: On", the preset names): requested from any thread, debounced, shown on the game
// thread through the game's region-entered notification (docs/design.md, "Banners"). Our own waiting entries are thinned
// out of the notification queue so presses cannot build a backlog.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "../../camera/api.hpp"
#include "../../common/live_ref.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace dw::smoothwalker::ui
{
    class Banner
    {
      public:
        // core: player_known() gates a request, player_controller() the show.
        explicit Banner(camera::CameraCore& core) : m_core(core) {}

        // The show_banner setting; any thread.
        auto enable(bool on) -> void { m_enabled.store(on); }

        // Debounced: a burst of presses shows one banner, with the last text.
        // Dropped without a player: a banner queued at the main menu would show minutes later, after a load.
        // Any thread.
        auto request(std::wstring text) -> void;

        // Game thread, every engine tick: shows the pending banner once its debounce has passed.
        auto show_pending() -> void;

      private:
        static constexpr auto BANNER_SETTLE = std::chrono::milliseconds(400);
        static constexpr const wchar_t* BANNER_PREFIX = L"Smoothwalker:";
        static constexpr int32_t BANNER_WORLD = 0x00;
        static constexpr int32_t BANNER_DATA = 0x08;
        static constexpr int32_t BANNER_TEXT = 0x20; // inside RegionData
        static constexpr int32_t BANNER_FLAG = 0x40;
        static constexpr size_t BANNER_PARAMS_SIZE = 0x48;

        struct ObjectArray
        {
            RC::Unreal::UObject** data;
            int32_t num;
            int32_t max;
        };

        auto drop_stale_banners(RC::Unreal::UObject* subsystem) -> void;
        auto is_our_banner(RC::Unreal::UObject* entry) -> bool;
        auto resolve_banner() -> bool;

        camera::CameraCore& m_core;
        std::mutex m_banner_mutex; // m_banner_text, m_banner_due, m_banner_pending
        std::wstring m_banner_text;
        std::chrono::steady_clock::time_point m_banner_due{};
        bool m_banner_pending = false;
        int32_t m_queue_offset = -1;        // NotificationSubsystem::NotificationQueue
        int32_t m_region_data_offset = -1;  // RegionEnteredNotificationInfo::RegionData
        RC::Unreal::UClass* m_region_info_class = nullptr;
        std::atomic<bool> m_enabled{true};
        int m_banner_state = 0; // 0 unresolved, 1 ready, -1 unavailable (game thread only)
        RC::Unreal::UFunction* m_banner_function = nullptr;
        RC::Unreal::UObject* m_banner_library = nullptr;
        dw::LiveRef m_notifications; // NotificationSubsystem, game thread only, checked live before use
    };
} // namespace dw::smoothwalker::ui
