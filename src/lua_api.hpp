// The Smoothwalker table handed to every Lua mod (docs/design.md, "Other camera mods and a camera API").
//
// Slice 1: register(), version fields, view(), live(), enabled(). Everything here reads atomics or a seqlock
// snapshot the hook publishes once per player-camera update, so these calls are safe from any thread: a mod
// may call them at its top level (UE4SS mod thread), from a key bind or hook (game thread), or from LoopAsync.
// Later slices that hand numbers to the hook will check the game thread and answer "bad_thread".
//
// Included from dllmain.cpp after the UE4SS headers: needs the Windows types they bring in.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <lua.hpp>

namespace dwapi
{
    constexpr int API_VERSION = 1;

    // Pitch, yaw, roll in degrees; FOV in degrees; location in cm, world space.
    struct View
    {
        double location[3];
        double rotation[3];
        double fov;
    };

    struct Snapshot
    {
        View game;        // the view as the game built it, before Smoothwalker
        View shown;       // what Smoothwalker handed back (equal to game when off)
        double pivot[3];  // the character pivot the follow is built around; NAN when unknown
        int64_t qpc;      // QueryPerformanceCounter at publish
    };

    // One writer (the hook, one player-camera update at a time), any number of readers.
    inline std::atomic<uint64_t> g_seq{0};
    inline Snapshot g_snapshot{};
    inline std::atomic<bool>* g_enabled = nullptr; // the mod's live switch, set by dllmain
    inline std::atomic<uint32_t> g_game_thread{0};  // captured on the engine tick; for later slices

    inline auto qpc_frequency() -> double
    {
        static const double f = [] {
            LARGE_INTEGER li{};
            QueryPerformanceFrequency(&li);
            return static_cast<double>(li.QuadPart);
        }();
        return f;
    }

    inline auto publish(const View& game, const View& shown, const double* pivot) -> void
    {
        Snapshot s{};
        s.game = game;
        s.shown = shown;
        for (int i = 0; i < 3; ++i) s.pivot[i] = pivot ? pivot[i] : NAN;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        s.qpc = now.QuadPart;
        auto v = g_seq.load(std::memory_order_relaxed);
        g_seq.store(v + 1, std::memory_order_release);
        std::memcpy(&g_snapshot, &s, sizeof(s));
        g_seq.store(v + 2, std::memory_order_release);
    }

    // False when nothing was published yet or a write was in flight on every try.
    inline auto read(Snapshot& out) -> bool
    {
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            auto a = g_seq.load(std::memory_order_acquire);
            if (a == 0 || (a & 1)) continue;
            std::memcpy(&out, &g_snapshot, sizeof(out));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_seq.load(std::memory_order_relaxed) == a) return true;
        }
        return false;
    }

    inline auto age_seconds(const Snapshot& s) -> double
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - s.qpc) / qpc_frequency();
    }

    constexpr double LIVE_WINDOW = 0.25; // s, the camera_live() window

    // ---------------------------------------------------------------------------------------------- Lua side

    struct Consumer
    {
        std::string mod;     // the Lua mod's folder name, from on_lua_start
        bool registered = false;
    };

    inline std::mutex g_mutex;
    inline std::unordered_map<lua_State*, Consumer> g_states; // keyed by each mod's main state
    inline std::string g_mod_version;

    inline auto main_state(lua_State* L) -> lua_State*
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        auto* main = lua_tothread(L, -1);
        lua_pop(L, 1);
        return main ? main : L;
    }

    inline auto consumer_of(lua_State* L) -> Consumer*
    {
        auto it = g_states.find(main_state(L));
        return it == g_states.end() ? nullptr : &it->second;
    }

    inline auto push_view(lua_State* L, const View& v) -> void
    {
        lua_createtable(L, 0, 7);
        lua_pushnumber(L, v.location[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, v.location[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, v.location[2]); lua_setfield(L, -2, "z");
        lua_pushnumber(L, v.rotation[0]); lua_setfield(L, -2, "pitch");
        lua_pushnumber(L, v.rotation[1]); lua_setfield(L, -2, "yaw");
        lua_pushnumber(L, v.rotation[2]); lua_setfield(L, -2, "roll");
        lua_pushnumber(L, v.fov); lua_setfield(L, -2, "fov");
    }

    // Smoothwalker.register() -> mod name. Idempotent; logs the consumer once.
    inline auto l_register(lua_State* L) -> int
    {
        std::lock_guard guard(g_mutex);
        auto* c = consumer_of(L);
        if (!c)
        {
            lua_pushnil(L);
            lua_pushstring(L, "unknown_state");
            return 2;
        }
        if (!c->registered)
        {
            c->registered = true;
            RC::Output::send<RC::LogLevel::Normal>(STR("[DWSmoothwalker] API consumer '{}' registered\n"), RC::to_wstring(c->mod));
        }
        lua_pushstring(L, c->mod.c_str());
        return 1;
    }

    // Smoothwalker.view() -> { game = {x,y,z,pitch,yaw,roll,fov}, shown = {...}, pivot = {x,y,z} | nil, age = s }
    // nil, "no_view" before the first player-camera update.
    inline auto l_view(lua_State* L) -> int
    {
        Snapshot s{};
        if (!read(s))
        {
            lua_pushnil(L);
            lua_pushstring(L, "no_view");
            return 2;
        }
        lua_createtable(L, 0, 4);
        push_view(L, s.game); lua_setfield(L, -2, "game");
        push_view(L, s.shown); lua_setfield(L, -2, "shown");
        if (std::isfinite(s.pivot[0]))
        {
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, s.pivot[0]); lua_setfield(L, -2, "x");
            lua_pushnumber(L, s.pivot[1]); lua_setfield(L, -2, "y");
            lua_pushnumber(L, s.pivot[2]); lua_setfield(L, -2, "z");
            lua_setfield(L, -2, "pivot");
        }
        lua_pushnumber(L, age_seconds(s)); lua_setfield(L, -2, "age");
        return 1;
    }

    // Smoothwalker.live() -> bool, age seconds (age is math.huge before the first update).
    inline auto l_live(lua_State* L) -> int
    {
        Snapshot s{};
        if (!read(s))
        {
            lua_pushboolean(L, 0);
            lua_pushnumber(L, HUGE_VAL);
            return 2;
        }
        double age = age_seconds(s);
        lua_pushboolean(L, age < LIVE_WINDOW ? 1 : 0);
        lua_pushnumber(L, age);
        return 2;
    }

    // Smoothwalker.enabled() -> bool: the mod's live switch (O, the ini, the Mod Menu).
    inline auto l_enabled(lua_State* L) -> int
    {
        lua_pushboolean(L, g_enabled && g_enabled->load(std::memory_order_relaxed) ? 1 : 0);
        return 1;
    }

    inline auto install(lua_State* L, std::string mod, std::string mod_version) -> void
    {
        std::lock_guard guard(g_mutex);
        g_mod_version = std::move(mod_version);
        g_states[main_state(L)] = Consumer{std::move(mod), false};
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, API_VERSION); lua_setfield(L, -2, "api_version");
        lua_pushstring(L, g_mod_version.c_str()); lua_setfield(L, -2, "mod_version");
        lua_pushcfunction(L, l_register); lua_setfield(L, -2, "register");
        lua_pushcfunction(L, l_view); lua_setfield(L, -2, "view");
        lua_pushcfunction(L, l_live); lua_setfield(L, -2, "live");
        lua_pushcfunction(L, l_enabled); lua_setfield(L, -2, "enabled");
        lua_setglobal(L, "Smoothwalker");
    }

    // A consumer may hold `local SW = Smoothwalker`, so the table is kept and every function in it is replaced
    // by a pure-Lua stub answering nil, "unloaded": no pointer into this DLL survives an unload or hot reload.
    inline auto uninstall(lua_State* L) -> void
    {
        std::lock_guard guard(g_mutex);
        g_states.erase(main_state(L));
        lua_getglobal(L, "Smoothwalker");
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            return;
        }
        if (luaL_dostring(L, "return function() return nil, 'unloaded' end") != LUA_OK)
        {
            lua_pop(L, 1); // the error
            lua_pushnil(L);
        }
        // stack: table, stub
        for (const char* name : {"register", "view", "live", "enabled"})
        {
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, name);
        }
        lua_pop(L, 1); // stub
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "api_version");
        lua_pop(L, 1); // table
    }

    // Every state still holding a live table: the unload path.
    inline auto uninstall_all() -> void
    {
        std::vector<lua_State*> states;
        {
            std::lock_guard guard(g_mutex);
            for (auto& [L, c] : g_states) states.push_back(L);
        }
        for (auto* L : states) uninstall(L);
    }
} // namespace dwapi
