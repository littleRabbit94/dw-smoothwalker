// Smoothwalker as a component of the one UE4SS mod (mod.cpp): the follow (a processor of the camera core),
// camera position in the game's modes, presets and slots, smoothwalker.ini, keys, banners and the debug overlay. It
// reaches the core only through its interface (camera/api.hpp) and registers its own UE4SS callbacks, apart from the
// core's (docs/design.md, "Core and processors"). Everything else of it is in smoothwalker.cpp.
#pragma once

#include "../camera/api.hpp"

#include <functional>
#include <memory>
#include <string>

namespace dw::smoothwalker
{
    class Smoothwalker
    {
      public:
        // Binds a key (a Windows virtual-key code) to an action on UE4SS's key thread: the mod's
        // register_keydown_event, which only the CppUserModBase can call.
        using BindKey = std::function<void(int key, std::function<void()> action)>;

        // Registers the follow with the core, reads smoothwalker.ini and the presets, logs the load line.
        Smoothwalker(camera::CameraCore& core, BindKey bind_key, std::wstring version);
        // Destroys the component. Not reached when a drain timed out: the mod then leaks this object and the core
        // (mod.cpp), since a stuck call may still read either.
        ~Smoothwalker();
        Smoothwalker(const Smoothwalker&) = delete;
        auto operator=(const Smoothwalker&) -> Smoothwalker& = delete;

        // on_unreal_init, once the core has hooked: the new-object callback and the keys.
        auto start() -> void;
        // on_update: the settings poll, the write-back, the log_stats report.
        auto update() -> void;

        // Unload, called by the mod's destructor in this order around the core's own steps (mod.cpp):
        auto unregister_callbacks() -> void; // UnregisterCallback waits for running callbacks
        auto shutdown() -> bool;             // after the core unhooked: modes restored, off the core; false: a
                                             // processor or listener call still runs after 5 s

      private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
} // namespace dw::smoothwalker
