// DWSmoothwalker: lags the character pivot the game's camera view is built around (GetCameraView, vtable
// slot 214). Design and measurements: docs/design.md.
//
// One UE4SS mod, one DLL, made of two components (docs/design.md, "Core and processors"):
// the camera core (camera/core.hpp: the hook, the player, cuts, the crossfade, layers, the Lua API) and Smoothwalker
// (smoothwalker/smoothwalker.hpp: the follow, camera position, presets, settings, keys, banners, the overlay),
// which reaches the core only through its interface (camera/api.hpp). This file is the glue: it owns both, forwards
// UE4SS's calls, and orders the unload between them.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "camera/api.hpp"
#include "camera/core.hpp"
#include "smoothwalker/smoothwalker.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>

using namespace RC;

namespace
{
    constexpr const char* LUA_MOD_VERSION = "0.11.0"; // Smoothwalker.mod_version
} // namespace

class DWSmoothwalker : public CppUserModBase
{
  public:
    DWSmoothwalker() : CppUserModBase()
    {
        m_core = std::make_unique<dw::camera::Core>(); // pins the DLL first, and logs a restart on it
        ModName = STR("DWSmoothwalker");
        ModVersion = STR("0.11.0");
        ModDescription = STR("Smoothed third-person camera follow");
        ModAuthors = STR("littleRabbit6");
        auto bind_key = [this](int key, std::function<void()> action) { register_keydown_event(static_cast<Input::Key>(key), std::move(action)); };
        m_smoothwalker = std::make_unique<dw::smoothwalker::Smoothwalker>(m_core->api(), bind_key, ModVersion);
    }

    // UE4SS FreeLibrary's the DLL right after this (hot reload); pinned, the image stays mapped (core.cpp,
    // pin_module). The Lua functions and the hook leave the core's slots first; UnregisterCallback waits for running
    // callbacks, so the game thread is out of both components before the modes are restored; the members go last,
    // and the next instance starts from fresh ones. A drain that timed out leaves a call running inside one of them,
    // and each reaches the other (the core calls the follow and the listener, Smoothwalker holds the core's
    // interface), so both are then leaked: no static points at either, and the next instance builds its own.
    ~DWSmoothwalker() override
    {
        const bool lua_drained = m_core->stop_lua();
        m_smoothwalker->unregister_callbacks();
        m_core->unregister_callbacks();
        const bool hook_drained = m_core->unhook();
        const bool smoothwalker_drained = m_smoothwalker->shutdown();
        if (lua_drained && hook_drained && smoothwalker_drained) return;

        std::wstring stuck;
        auto add = [&](bool drained, const wchar_t* what) {
            if (drained) return;
            if (!stuck.empty()) stuck += L", ";
            stuck += what;
        };
        add(lua_drained, L"a Lua call in the camera API");
        add(hook_drained, L"a camera update in the hook");
        add(smoothwalker_drained, L"a core call in the follow or the listener");
        Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unload: still running after 5 s: {}; both components left allocated\n"), stuck);
        (void)m_smoothwalker.release();
        (void)m_core.release();
    }

    auto on_lua_start(StringViewType mod_name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override
    {
        m_core->lua_start(lua.get_lua_state(), to_string(mod_name), LUA_MOD_VERSION);
    }

    auto on_lua_stop(StringViewType, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override
    {
        m_core->lua_stop(lua.get_lua_state());
    }

    auto on_unreal_init() -> void override
    {
        if (!m_core->start()) return; // no hook: neither component registers anything
        m_smoothwalker->start();
    }

    auto on_update() -> void override
    {
        m_smoothwalker->update();
    }

  private:
    std::unique_ptr<dw::camera::Core> m_core;
    std::unique_ptr<dw::smoothwalker::Smoothwalker> m_smoothwalker;
};

#define DW_SMOOTHWALKER_API __declspec(dllexport)
extern "C"
{
    DW_SMOOTHWALKER_API CppUserModBase* start_mod()
    {
        return new DWSmoothwalker();
    }

    DW_SMOOTHWALKER_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
