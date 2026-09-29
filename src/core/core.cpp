// The camera core's side of the DLL (docs/design.md, "Core and processors"): installs the GetCameraView
// hook on vtable slot 214 (core/pipeline.hpp), finds the player, its pawn and its camera, injects the Lua API
// (core/lua_api.hpp) and hands out the C table (core/api.hpp). Includes no Smoothwalker header.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "pipeline.hpp"
#include "core.hpp"
#include "../live_ref.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>

using namespace RC;
using namespace RC::Unreal;

extern "C" const dwcam::Api* dwcc_get_api(uint32_t version)
{
    return dwcam::get_api(version);
}

namespace dwcam
{
namespace
{
    auto object_ptr(UObject* owner, const TCHAR* property) -> UObject*
    {
        if (!owner) return nullptr;
        auto** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(property);
        return value ? *value : nullptr;
    }

    // Pins this DLL for the life of the process. A hot reload FreeLibrary's it right after the destructor, but
    // UnregisterCallback only marks the Hook callbacks dead: UE4SS destroys their std::function later, on its
    // callback GC thread (one detour per 3 s pass) or when a reader drops its snapshot. With the image unmapped that
    // destructor read a freed vtable (AV at UE4SS.dll+0x43B83A, RTTI: the on_unreal_init BeginPlay lambda). Pinned,
    // FreeLibrary leaves the image mapped and the next LoadLibrary returns it: no new code, and no static
    // initializer runs again (reset_globals). docs/design.md, "The DLL is pinned".
    auto pin_module() -> void
    {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&get_camera_view_hook), &self))
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not pin the DLL (error {}): a hot reload may crash in UE4SS\n"), GetLastError());
        }
    }
} // namespace

struct Core::Impl
{
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    bool m_hooked = false; // this instance installed the slot 214 hook or kept the previous instance's
    FName m_player_controller_name{};
    // Game thread only, each checked against the object array every engine tick before use. m_controller is
    // mirrored in g_player_controller and g_player_known for the table (hold_controller).
    dwsc::LiveRef m_controller, m_pawn, m_camera, m_root;
    int32_t m_pawn_offset = -1; // AController::Pawn, same class every map
    bool m_offset_retry = false; // find_translation_offset failed for m_pawn; game thread only
    std::chrono::steady_clock::time_point m_next_offset_scan{};
    std::chrono::seconds m_offset_wait{2};
    std::atomic<bool> m_find_requested{false};
    std::chrono::steady_clock::time_point m_next_find{};
    std::chrono::seconds m_find_interval{2}; // FindFirstOf fallback: 2 s, doubling to 60 s while nothing is found
    std::mutex m_new_controller_mutex;          // m_new_controller: written by the new-object callback on any thread
    dwsc::LiveRef m_new_controller;
    std::atomic<bool> m_new_controller_pending{false};
    int32_t m_viewport_offset = -1, m_world_offset = -1; // UEngine::GameViewport, UGameViewportClient::World; -2 absent
    UObject* m_world = nullptr;                 // compared only, never followed
    bool m_world_seen = false;

    auto hold_controller(dwsc::LiveRef controller) -> void
    {
        m_controller = controller;
        g_player_controller.store(controller.object);
        g_player_known.store(controller.object != nullptr);
    }

    auto install_hook() -> bool
    {
        auto* camera = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__CameraComponent"));
        auto* rebel = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/RebelCamera.Default__RebelCameraComponent"));
        if (!camera || !rebel)
        {
            Output::send<LogLevel::Error>(STR("[DWSmoothwalker] camera class defaults not found, mod inactive\n"));
            return false;
        }
        auto** base = *reinterpret_cast<uintptr_t***>(camera);
        auto** vtable = *reinterpret_cast<uintptr_t***>(rebel);
        if (base[GET_CAMERA_VIEW_SLOT] == vtable[GET_CAMERA_VIEW_SLOT])
        {
            Output::send<LogLevel::Error>(STR("[DWSmoothwalker] slot {} not overridden: unsupported game build, mod inactive\n"), GET_CAMERA_VIEW_SLOT);
            return false;
        }

        auto** entry = &vtable[GET_CAMERA_VIEW_SLOT];
        auto* hook = reinterpret_cast<uintptr_t*>(&get_camera_view_hook);
        // A hot reload restarts on the same pinned image, and the last unload may have left the hook in place: it
        // found another mod's hook over ours (g_hook_left), or that mod has since unhooked and put ours back.
        // g_original then still holds what we called before. Capturing the slot again while our hook is reached
        // would store the hook itself, or the other mod's hook that calls it, as the original: endless recursion on
        // the first camera update. So the previous original is kept, except where the slot provably bypasses us:
        // it holds g_original itself (a mod hooked below us wrote it back at its unload, then hooked again at the
        // same address, or never hooked again), and nothing reached from the slot is known to call our hook, so a
        // fresh capture is taken. A mod below us that captured our hook as its own original (one above us put ours
        // back first) still loops; so did the code before the pin.
        const bool bypassed = g_hook_left && *entry != hook && reinterpret_cast<GetCameraViewFn>(*entry) == g_original;
        if (bypassed) g_hook_left = false;
        if (!bypassed && (*entry == hook || g_hook_left))
        {
            // Unmapped: the mod we called was freed. Keeping it crashes, recapturing may recurse; neither is safe.
            // Refusing only stops the log from claiming success: a hook still in the chain calls the freed original
            // anyway. A trampoline outside any module (a hooking library's VirtualAlloc) also reads as unmapped.
            HMODULE owner{};
            bool mapped = g_original && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                           reinterpret_cast<LPCWSTR>(g_original), &owner);
            if (!mapped || g_original == &get_camera_view_hook || g_vtable_entry != entry)
            {
                Output::send<LogLevel::Error>(STR("[DWSmoothwalker] slot {} was left hooked by the last unload and no usable original is known; "
                                                  "smoothing off, restart the game\n"),
                                              GET_CAMERA_VIEW_SLOT);
                return false;
            }
            m_hooked = true;
            if (*entry == hook)
            {
                Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] GetCameraView hook kept from before the reload (slot {})\n"), GET_CAMERA_VIEW_SLOT);
            }
            else
            {
                // Another pointer: whether it still calls ours cannot be seen from here.
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] slot {} holds another mod's hook: assuming it still calls this mod's hook from "
                                                    "before the reload. If smoothing does not work, restart the game to hook cleanly\n"),
                                                GET_CAMERA_VIEW_SLOT);
            }
            return true;
        }
        DWORD prev{};
        if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &prev))
        {
            Output::send<LogLevel::Error>(STR("[DWSmoothwalker] vtable protect failed, mod inactive\n"));
            return false;
        }
        g_original = reinterpret_cast<GetCameraViewFn>(*entry);
        *entry = hook;
        VirtualProtect(entry, sizeof(*entry), prev, &prev);
        g_vtable_entry = entry;
        m_hooked = true;
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] GetCameraView hooked (slot {})\n"), GET_CAMERA_VIEW_SLOT);
        return true;
    }

    auto forget_player(dwsc::Snap why = dwsc::Snap::Player) -> void
    {
        g_player_camera.store(nullptr);
        g_player_root.store(nullptr);
        request_cut(why);
        m_pawn = m_camera = m_root = {};
        hold_controller({});
    }

    // A level change, called from the LoadMap pre hook and the engine tick; harmless if run twice for one load.
    auto forget_world() -> void
    {
        forget_player(dwsc::Snap::World);
        // Smoothwalker drops its camera-mode pointers, re-applies in the next world and rebuilds its overlay.
        notify([](const Listener& l) {
            if (l.world_changed) l.world_changed(l.user);
        });
    }

    // A duplicate of the new-object path where UE4SS installs BeginPlay. Game thread.
    auto on_begin_play(AActor* actor) -> void
    {
        auto* object = static_cast<UObject*>(actor);
        if (!object || m_controller.alive()) return;
        auto* cls = object->GetClassPrivate();
        if (cls && cls->GetNamePrivate() == m_player_controller_name) adopt_controller(dwsc::LiveRef::of(object));
    }

    auto on_end_play(AActor* actor) -> void
    {
        auto* object = static_cast<UObject*>(actor);
        if (object && object == m_controller.object) forget_player();
        else if (object && object == m_pawn.object) forget_pawn();
    }

    auto forget_pawn() -> void
    {
        g_player_camera.store(nullptr);
        g_player_root.store(nullptr);
        request_cut(dwsc::Snap::Pawn);
        m_pawn = m_camera = m_root = {};
    }

    auto adopt_controller(dwsc::LiveRef controller) -> void
    {
        forget_player();
        hold_controller(controller);
        if (g_log_verbose.load(std::memory_order_relaxed)) Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] player controller found\n"));
    }

    // GEngine->GameViewport->World, both reflected properties (not hard offsets); compared only, never followed.
    auto check_world(UEngine* engine) -> void
    {
        auto* object = static_cast<UObject*>(engine);
        if (!object || m_viewport_offset == -2 || m_world_offset == -2) return;
        auto offset_of = [](UObject* owner, const TCHAR* name) -> int32_t {
            auto** slot = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
            return slot ? static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(owner)) : -2;
        };
        if (m_viewport_offset < 0) m_viewport_offset = offset_of(object, STR("GameViewport"));
        if (m_viewport_offset < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] UEngine has no GameViewport property: level changes rely on the LoadMap hook\n"));
            return;
        }
        auto* viewport = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(object) + m_viewport_offset);
        UObject* world = nullptr;
        if (viewport)
        {
            if (m_world_offset < 0) m_world_offset = offset_of(viewport, STR("World"));
            if (m_world_offset < 0)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] GameViewportClient has no World property: level changes rely on the LoadMap hook\n"));
                return;
            }
            world = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(viewport) + m_world_offset);
        }
        if (m_world_seen && world == m_world) return;
        bool first = !m_world_seen;
        m_world_seen = true;
        m_world = world;
        if (first) return;
        forget_world();
        rescan_now(); // the new world's controller: look now if the new-object hand-off missed it
    }

    auto rescan_now() -> void
    {
        m_next_find = {};
        m_find_interval = std::chrono::seconds(2);
    }

    // The new-object hand-off (installed by every UE4SS profile) is primary; FindFirstOf is the fallback for a hot
    // reload or a missed hand-off, backing off per m_find_interval so the main menu is not scanned every tick.
    // Candidates are adopted only if the object array still holds them.
    auto discover_controller() -> void
    {
        if (m_new_controller_pending.exchange(false))
        {
            dwsc::LiveRef candidate;
            {
                std::lock_guard guard(m_new_controller_mutex);
                candidate = std::exchange(m_new_controller, {});
            }
            if (!m_controller.alive() && candidate.alive()) adopt_controller(candidate);
        }
        if (m_controller.object) return;

        auto now = std::chrono::steady_clock::now();
        bool requested = m_find_requested.exchange(false);
        if (!requested && now < m_next_find) return;
        m_next_find = now + m_find_interval;
        m_find_interval = std::min(m_find_interval * 2, std::chrono::seconds(60));
        auto candidate = dwsc::LiveRef::of(UObjectGlobals::FindFirstOf(STR("BP_PlayerController_C")));
        if (candidate.object && !candidate.object->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) &&
            candidate.alive())
        {
            adopt_controller(candidate);
        }
        else if (requested && g_log_verbose.load(std::memory_order_relaxed))
        {
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] player controller not found yet\n"));
        }
    }

    // Pointer reads and object array lookups only, before anything reads through a possibly-freed held pointer.
    auto on_engine_tick(UEngine* engine) -> void
    {
        if (dwapi::g_game_thread.load(std::memory_order_relaxed) == 0) dwapi::g_game_thread.store(GetCurrentThreadId());
        check_world(engine);
        if (m_controller.object && !m_controller.alive())
        {
            if (g_log_verbose.load(std::memory_order_relaxed)) Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] player controller gone\n"));
            forget_player();
            rescan_now();
        }
        else if (m_pawn.object && !(m_pawn.alive() && (!m_camera.object || m_camera.alive()) && (!m_root.object || m_root.alive())))
        {
            forget_pawn();
        }
        discover_controller();

        // Smoothwalker's banners, camera-mode tuning and overlay, with the controller checked and before the pawn is.
        notify([](const Listener& l) {
            if (l.tick) l.tick(l.user);
        });
        if (!m_controller.object) return;

        // Name lookup once per controller class, then a plain read.
        if (m_pawn_offset < 0)
        {
            auto** slot = m_controller.object->GetValuePtrByPropertyNameInChain<UObject*>(STR("Pawn"));
            if (!slot)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] controller has no Pawn property\n"));
                hold_controller({});
                return;
            }
            m_pawn_offset = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(m_controller.object));
        }
        auto* pawn = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(m_controller.object) + m_pawn_offset);
        if (pawn == m_pawn.object && !m_offset_retry) return;
        // The translation scan failed for this pawn (at the origin the triple matches twice): retry, backing off.
        if (pawn == m_pawn.object && std::chrono::steady_clock::now() < m_next_offset_scan) return;
        if (pawn != m_pawn.object) m_offset_wait = std::chrono::seconds(2); // a new pawn does not inherit the old one's wait
        m_offset_retry = false;
        forget_pawn();
        m_pawn = dwsc::LiveRef::of(pawn);
        // Controller.Pawn can point at a Garbage pawn until GC; re-adopting it would warn every tick.
        if (!m_pawn.alive()) return;

        auto camera = dwsc::LiveRef::of(object_ptr(pawn, STR("FollowCamera")));
        auto root = dwsc::LiveRef::of(object_ptr(pawn, STR("RootComponent")));
        // m_pawn stays set on failure, so a pawn without them is reported once, not every tick.
        if (!camera.alive() || !root.alive())
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] pawn {} has no FollowCamera or RootComponent\n"), pawn->GetName());
            return;
        }
        if (g_translation_offset.load() < 0 && !find_translation_offset(root.object))
        {
            m_offset_retry = true;
            m_next_offset_scan = std::chrono::steady_clock::now() + m_offset_wait;
            m_offset_wait = std::min(m_offset_wait * 2, std::chrono::seconds(60)); // each miss logs a warning
            return;
        }

        // The root is the capsule: its half height gives the feet point the vertical follow tracks.
        if (auto* half = root.object->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight")))
        {
            g_half_height_offset.store(static_cast<int32_t>(reinterpret_cast<uint8_t*>(half) - reinterpret_cast<uint8_t*>(root.object)));
        }
        else
        {
            g_half_height_offset.store(-1);
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] CapsuleHalfHeight not found: the vertical follow tracks the capsule centre\n"));
        }

        m_camera = camera;
        m_root = root;
        g_player_root.store(root.object);
        g_player_camera.store(camera.object);
        // Smoothwalker: a new pawn's modes get the current camera position, found by a fresh scan.
        notify([](const Listener& l) {
            if (l.camera_changed) l.camera_changed(l.user);
        });
        m_offset_wait = std::chrono::seconds(2);
        if (g_log_verbose.load(std::memory_order_relaxed))
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] following {} (CapsuleHalfHeight at 0x{:X})\n"), pawn->GetName(), g_half_height_offset.load());
    }

    // ComponentToWorld is not reflected. A root's world translation equals its RelativeLocation, so the offset
    // is the one other place those doubles sit with a unit scale 0x20 after (UE 5.5 FTransform layout).
    auto find_translation_offset(UObject* root) -> bool
    {
        auto* relative = root->GetValuePtrByPropertyNameInChain<double>(STR("RelativeLocation"));
        if (!relative)
        {
            Output::send<LogLevel::Error>(STR("[DWSmoothwalker] RelativeLocation not found, smoothing inactive\n"));
            return false;
        }
        auto base = reinterpret_cast<uint8_t*>(root);
        auto relative_offset = reinterpret_cast<uint8_t*>(relative) - base;
        double want[3]{relative[0], relative[1], relative[2]};

        int32_t found = -1;
        int matches = 0;
        for (int32_t offset = 0x28; offset + 0x40 <= 0x800; offset += 8)
        {
            if (offset == relative_offset) continue;
            double block[7]{};
            if (!guarded_read(base + offset, block, sizeof(block))) break;
            bool location = std::abs(block[0] - want[0]) < 0.5 && std::abs(block[1] - want[1]) < 0.5 && std::abs(block[2] - want[2]) < 0.5;
            bool scale = std::abs(block[4] - 1.0) < 1e-3 && std::abs(block[5] - 1.0) < 1e-3 && std::abs(block[6] - 1.0) < 1e-3;
            if (location && scale)
            {
                found = offset;
                ++matches;
            }
        }
        if (matches != 1)
        {
            Output::send<LogLevel::Error>(STR("[DWSmoothwalker] ComponentToWorld translation: {} matches, smoothing inactive\n"), matches);
            return false;
        }
        g_translation_offset.store(found);
        if (g_log_verbose.load(std::memory_order_relaxed))
            Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] ComponentToWorld translation at 0x{:X} (RelativeLocation 0x{:X})\n"), found,
                                           relative_offset);
        return true;
    }
};

Core::Core() : m(std::make_unique<Impl>())
{
    pin_module();
    if (g_starts++ > 0)
    {
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] hot reload: restarted on the DLL already loaded (pinned); a rebuilt DLL needs the game restarted\n"));
    }
    dwapi::g_enabled = &processor_enabled;
    dwapi::g_reset = &g_reset; // release("cut") snaps through the same flag a teleport sets
    dwapi::g_reset_reason = &g_reset_reason;
    QueryPerformanceFrequency(&g_qpc_frequency);
}

Core::~Core() = default;

auto Core::start() -> bool
{
    m->m_player_controller_name = FName(STR("BP_PlayerController_C"), FNAME_Add);

    if (!m->install_hook()) return false;

    // UE4SS only installs BeginPlay, EndPlay and LoadMap when [Hooks] enables them (off in a "Performance"
    // profile); StaticConstructObject is always installed. Registering on an uninstalled hook logs an error,
    // so each optional hook is guarded by its flag; the engine tick covers world change, liveness and lookup.
    Hook::FCallbackOptions options{false, true, STR("DWSmoothwalker"), STR("")};
    auto& hooks = UnrealInitializer::StaticStorage::GlobalConfig;
    Impl* self = m.get();
    auto add = [&](Hook::GlobalCallbackId id) {
        if (id != Hook::ERROR_ID) self->m_callbacks.push_back(id);
        return id != Hook::ERROR_ID;
    };
    bool begin_play = hooks.bHookBeginPlay &&
                      add(Hook::RegisterBeginPlayPostCallback([self](auto&, AActor* actor) { self->on_begin_play(actor); }, options));
    bool end_play = hooks.bHookEndPlay &&
                    add(Hook::RegisterEndPlayPostCallback([self](auto&, AActor* actor, EEndPlayReason) { self->on_end_play(actor); }, options));
    bool load_map = hooks.bHookLoadMap && add(Hook::RegisterLoadMapPreCallback(
                                                  [self](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) { self->forget_world(); },
                                                  options));
    // Runs on whatever thread constructs the object (async loading threads too): only a flag test, a pointer and an
    // FName compare and, on a match, a locked hand-off of the controller into m_new_controller. Camera modes pushed
    // on the player's camera are Smoothwalker's, from its own callback.
    bool new_object = add(Hook::RegisterStaticConstructObjectPostCallback(
            [self](auto& info, const FStaticConstructObjectParameters& params) {
                if (static_cast<uint32_t>(params.SetFlags) & static_cast<uint32_t>(RF_ClassDefaultObject | RF_ArchetypeObject)) return;
                auto* cls = const_cast<UClass*>(params.Class);
                if (!cls) return;
                if (cls->GetNamePrivate() != self->m_player_controller_name) return;
                auto* object = info.GetCurrentResolvedReturnValue();
                if (!object) return;
                std::lock_guard guard(self->m_new_controller_mutex);
                self->m_new_controller = dwsc::LiveRef::of(object);
                self->m_new_controller_pending.store(true);
            }, options));
    bool engine_tick =
            hooks.bHookEngineTick && add(Hook::RegisterEngineTickPostCallback([self](auto&, UEngine* engine, float, bool) { self->on_engine_tick(engine); }, options));
    auto state = [](bool on) { return on ? STR("on") : STR("off"); };
    if (g_log_verbose.load(std::memory_order_relaxed))
    {
        Output::send<LogLevel::Verbose>(STR("[DWSmoothwalker] player discovery: new-object callback {}, BeginPlay {}, EndPlay {}, LoadMap {}; engine tick "
                                           "checks world and liveness, FindFirstOf fallback from 2 s backing off to 60 s without a controller\n"),
                                       state(new_object), state(begin_play), state(end_play), state(load_map));
    }
    if (!engine_tick) Output::send<LogLevel::Error>(STR("[DWSmoothwalker] UE4SS EngineTick hook is off: the camera cannot find the player\n"));

    m->m_find_requested.store(true); // after a hot reload the controller has already begun play
    return true;
}

// The Smoothwalker table into every Lua mod's state as it starts (lua_api.hpp). Fires for each Lua mod because
// C++ mods are started first (docs/design.md, "Checks run 2026-09-22").
auto Core::lua_start(lua_State* L, const std::string& mod, const std::string& mod_version) -> void
{
    dwapi::install(L, mod, mod_version);
}

auto Core::lua_stop(lua_State* L) -> void
{
    dwapi::uninstall(L);
}

auto Core::stop_lua() -> void
{
    dwapi::uninstall_all();
}

auto Core::unregister_callbacks() -> void
{
    for (auto id : m->m_callbacks) Hook::UnregisterCallback(id);
    m->m_callbacks.clear();
}

// UE4SS FreeLibrary's the DLL right after the mod's destructor (hot reload); pinned, the image stays mapped
// (pin_module). A call already in the hook must return before the globals are reset.
auto Core::unhook() -> void
{
    // From here a camera update that still reaches the hook returns right after the original.
    g_player_camera.store(nullptr);
    g_player_root.store(nullptr);
    if (!m->m_hooked) return;
    g_hook_left = true; // until the restore below succeeds
    DWORD prev{};
    if (VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), PAGE_READWRITE, &prev))
    {
        // Only this mod's own entry is put back: another mod hooked after us would otherwise be unhooked too.
        // Left in place, the hook stays callable through that mod (the image is pinned) and the next start
        // keeps g_original (install_hook).
        if (*g_vtable_entry == reinterpret_cast<uintptr_t*>(&get_camera_view_hook))
        {
            *g_vtable_entry = reinterpret_cast<uintptr_t*>(g_original);
            g_hook_left = false;
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unload: slot {} no longer holds this mod's hook, left as is\n"), GET_CAMERA_VIEW_SLOT);
        }
        VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), prev, &prev);
    }
    // A worker may have read the old entry just before the restore and not entered the hook yet.
    Sleep(50);
    for (int i = 0; i < 500 && g_in_hook.load() != 0; ++i) Sleep(10);
    if (g_in_hook.load() != 0) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unload: a camera update is still in the hook\n"));
}

// Every namespace-scope variable of the core holding per-instance state, back to its static-init value. The image
// is pinned, so the next instance after a hot reload starts on it with no static initializer run: whatever is left
// out here carries the previous instance's state into it. Any new namespace-scope, class-static or function-local
// static that holds state goes here (docs/design.md, "The DLL is pinned"). Kept on purpose: g_original,
// g_vtable_entry and g_hook_left (install_hook), g_in_hook, g_in_processor and g_in_listener (self-balancing, and a
// call through a hook left in the chain may be in flight), g_starts, the locks, constants, and the QPC frequency
// (per boot; the constructor queries it again). Called last by the mod's destructor: g_player_camera is already
// null and the in-hook wait is done, so a hook that is still reached returns after the original and never touches
// any of this. Smoothwalker's side resets its own (sw/smoothwalker.cpp, shutdown).
auto Core::reset_globals() -> void
{
    AcquireSRWLockExclusive(&g_tuning_lock);
    g_tuning = DEFAULT_TUNING;
    ReleaseSRWLockExclusive(&g_tuning_lock);
    g_reset.store(true);
    g_reset_reason.store(static_cast<int>(dwsc::Snap::Startup));
    g_hook_timing.store(false);
    g_log_verbose.store(false);
    g_player_camera.store(nullptr);
    g_player_root.store(nullptr);
    g_player_controller.store(nullptr);
    g_player_known.store(false);
    g_translation_offset.store(-1);
    g_half_height_offset.store(-1);
    g_view_updates.store(0);
    g_last_view_qpc.store(0);
    g_view_seconds.store(0.0);
    g_calls_timed.store(0);
    g_ticks_spent.store(0);
    g_debug_keep_follow.store(NAN);
    g_debug_keep_turn.store(NAN);
    g_debug_influence.store(0);
    g_debug_lag_h.store(NAN);
    g_debug_lag_v.store(NAN);
    g_debug_rate_h.store(NAN);
    g_debug_snap.store(0);
    g_debug_snap_qpc.store(0);
    g_debug_glide.store(false);
    g_view = ViewState{};
    g_processor.store(nullptr);
    g_listener.store(nullptr);
    dwapi::reset_state();
}
} // namespace dwcam
