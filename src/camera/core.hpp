// The camera core as a component of the one UE4SS mod (mod.cpp): the slot 214 hook and its pin, player and
// camera discovery, the Lua injection, and the interface the Smoothwalker side uses (camera/api.hpp, CameraCore).
// It registers its own UE4SS callbacks, apart from Smoothwalker's (docs/design.md, "Core and processors"), and owns
// the Pipeline and the Authority, which the hook and the Lua functions reach through image-level slots while it lives.
// Everything else of it is in core.cpp and the camera files it includes: hook, pipeline, authority, lua_api.
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
        // Pins the DLL, logs a restart on the image already loaded, publishes the Pipeline to the hook and the
        // Authority to the Lua functions.
        Core();
        ~Core();
        Core(const Core&) = delete;
        auto operator=(const Core&) -> Core& = delete;

        // on_unreal_init: the hook, then the core's UE4SS callbacks and the first controller lookup. False: slot
        // 214 could not be hooked, nothing was registered, and the mod is inactive.
        auto start() -> bool;

        // The core's interface, for the Smoothwalker side. Valid for the life of this Core.
        auto api() -> CameraCore&;

        // on_lua_start / on_lua_stop: the Smoothwalker table into each Lua mod's state, and out of it.
        auto lua_start(lua_State* L, const std::string& mod, const std::string& mod_version) -> void;
        auto lua_stop(lua_State* L) -> void;

        // Unload, called by the mod's destructor in this order around Smoothwalker's own steps (mod.cpp). A false
        // return: a call is still running inside the Core after 5 s, so the Core must not be destroyed.
        auto stop_lua() -> bool;             // every Lua table left holds stubs; Lua calls in flight waited for
        auto unregister_callbacks() -> void; // UnregisterCallback waits for running callbacks
        auto unhook() -> bool;               // player camera null, slot 214 restored, hook calls in flight waited for

      private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
} // namespace dw::camera
