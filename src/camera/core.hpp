// The camera core as a component of the one UE4SS mod (mod.cpp): the slot 214 hook and its pin, player and
// camera discovery, the Lua injection, and the C table the Smoothwalker side uses (camera/api.hpp, dwcc_get_api).
// It registers its own UE4SS callbacks, apart from Smoothwalker's (docs/design.md, "Core and processors").
// Everything else of it is in core.cpp and camera/pipeline.hpp.
#pragma once

#include "api.hpp"

#include <memory>
#include <string>

struct lua_State;

namespace dw::camera
{
    class Core
    {
      public:
        // Pins the DLL, logs a restart on the image already loaded, reads the QPC frequency.
        Core();
        ~Core();
        Core(const Core&) = delete;
        auto operator=(const Core&) -> Core& = delete;

        // on_unreal_init: the hook, then the core's UE4SS callbacks and the first controller lookup. False: slot
        // 214 could not be hooked, nothing was registered, and the mod is inactive.
        auto start() -> bool;

        // on_lua_start / on_lua_stop: the Smoothwalker table into each Lua mod's state, and out of it.
        auto lua_start(lua_State* L, const std::string& mod, const std::string& mod_version) -> void;
        auto lua_stop(lua_State* L) -> void;

        // Unload, called by the mod's destructor in this order around Smoothwalker's own steps (mod.cpp):
        auto stop_lua() -> void;             // every Lua table left holds stubs
        auto unregister_callbacks() -> void; // UnregisterCallback waits for running callbacks
        auto unhook() -> void;               // player camera null, slot 214 restored, in-hook calls waited for
        auto reset_globals() -> void;        // last: every per-instance global back to its static-init value

      private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
} // namespace dw::camera
