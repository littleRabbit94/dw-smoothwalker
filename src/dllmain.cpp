// DWSmoothwalker: lags the character pivot the game's camera view is built around (GetCameraView, vtable
// slot 214). Design and measurements: docs/design.md.
//
// One UE4SS mod, one DLL, made of two components (docs/design.md, "Core and processors"):
// the camera core (core/core.hpp: the hook, the player, cuts, the crossfade, layers, the Lua API) and Smoothwalker
// (sw/smoothwalker.hpp: the follow, camera position, presets, settings, keys, banners, the overlay), which reaches
// the core only through its C table (core/api.hpp). This file is the glue: it owns both, forwards UE4SS's calls,
// and orders the unload between them.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "core/api.hpp"
#include "core/core.hpp"
#include "sw/smoothwalker.hpp"

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
    constexpr const char* LUA_MOD_VERSION = "0.10.1"; // Smoothwalker.mod_version
} // namespace

class DWSmoothwalker : public CppUserModBase
{
  public:
    DWSmoothwalker() : CppUserModBase()
    {
        m_core = std::make_unique<dwcam::Core>(); // pins the DLL first, and logs a restart on it
        ModName = STR("DWSmoothwalker");
        ModVersion = STR("0.10.1");
        ModDescription = STR("Frame-interpolated third-person camera");
        ModAuthors = STR("littleRabbit6");
        auto bind_key = [this](int key, std::function<void()> action) { register_keydown_event(static_cast<Input::Key>(key), std::move(action)); };
        m_smoothwalker = std::make_unique<dwsw::Smoothwalker>(*dwcc_get_api(dwcam::API_VERSION), bind_key, ModVersion);
    }

    // UE4SS FreeLibrary's the DLL right after this (hot reload); pinned, the image stays mapped (core.cpp,
    // pin_module). UnregisterCallback waits for running callbacks, so the game thread is out of both components
    // before the modes are restored; a call already in the hook returns before any global is reset.
    ~DWSmoothwalker() override
    {
        m_core->stop_lua();
        m_smoothwalker->unregister_callbacks();
        m_core->unregister_callbacks();
        m_core->unhook();
        m_smoothwalker->shutdown();
        m_core->reset_globals();
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
    std::unique_ptr<dwcam::Core> m_core;
    std::unique_ptr<dwsw::Smoothwalker> m_smoothwalker;
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
