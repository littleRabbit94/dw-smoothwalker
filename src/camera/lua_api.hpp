// The Smoothwalker table handed to every Lua mod (docs/design.md, "Other camera mods and a camera API"): argument
// parsing and Lua pushes over the API's state, which camera/authority.hpp holds.
//
// Slice 1: register(), version fields, view(), live(), enabled(). These read atomics or a seqlock snapshot the
// hook publishes once per player-camera update, so they are safe from any thread: a mod may call them at its
// top level (UE4SS mod thread), from a key bind or hook (game thread), or from LoopAsync.
// Slice 2: layer_set(), layer_clear(), layers(). One override layer per consumer, applied in the hook after the
// follow and the wall clamp. These write state the hook reads and are game thread only ("bad_thread").
// Slice 3: claim(), release(mode), owner(). One camera owner at a time. While a claim is held the follow math
// keeps running on the game's view and the hook writes nothing, so a release can cut or glide back. A claim may
// take a ttl lease, so a consumer that crashes mid-claim cannot hold the camera forever. claim() and release()
// are game thread only; owner() reads under the Authority's mutex and is thread-agnostic.
//
// Included by camera/core.cpp only: needs the Windows types included before it.
#pragma once

#include "authority.hpp"
#include "snapshot.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <lua.hpp>

namespace dw::camera::lua
{
    constexpr int API_VERSION = 4; // 4: a repeat claim with a ttl renews the lease

    // A consumer's key in the Authority: its main state.
    inline auto main_state(lua_State* L) -> lua_State*
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        auto* main = lua_tothread(L, -1);
        lua_pop(L, 1);
        return main ? main : L;
    }

    inline auto fail(lua_State* L, const char* reason) -> int
    {
        lua_pushnil(L);
        lua_pushstring(L, reason);
        return 2;
    }

    // The reason string Lua gets for a refusal; nullptr for Ok.
    inline auto reason_of(ApiResult result) -> const char*
    {
        switch (result)
        {
        case ApiResult::Ok: return nullptr;
        case ApiResult::UnknownState: return "unknown_state";
        case ApiResult::AlreadyYours: return "already_yours";
        case ApiResult::Taken: return "taken";
        case ApiResult::MustKeep: return "must_keep";
        case ApiResult::NotOwner: return "not_owner";
        case ApiResult::NoSlot: return "no_slot";
        }
        return "unknown_state";
    }

    // nullptr when the caller is on the game thread, else the reason to answer.
    inline auto wrong_thread() -> const char*
    {
        auto game = g_authority.game_thread();
        if (game == 0) return "no_game_thread";
        return GetCurrentThreadId() == game ? nullptr : "bad_thread";
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
    // Every Lua-facing function has the Authority finish its bookkeeping (under its mutex, into locals) before any
    // lua_* call that can raise: a Lua error longjmps out of a C function, and one thrown under the lock would leave
    // it held and deadlock every later API call.
    inline auto l_register(lua_State* L) -> int
    {
        std::string mod;
        bool first = false;
        if (auto why = reason_of(g_authority.register_consumer(main_state(L), mod, first))) return fail(L, why);
        if (first) RC::Output::send<RC::LogLevel::Normal>(STR("[DWSmoothwalker] API consumer '{}' registered\n"), RC::to_wstring(mod));
        lua_pushstring(L, mod.c_str());
        return 1;
    }

    // Smoothwalker.view() -> { game = {x,y,z,pitch,yaw,roll,fov}, shown = {...}, pivot = {x,y,z} | nil, age = s }
    // nil, "no_view" before the first player-camera update.
    inline auto l_view(lua_State* L) -> int
    {
        Snapshot s{};
        if (!g_authority.read_view(s)) return fail(L, "no_view");
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
        lua_pushnumber(L, age_seconds(s, g_authority.clock())); lua_setfield(L, -2, "age");
        return 1;
    }

    // Smoothwalker.live() -> bool, age seconds (age is math.huge before the first update).
    inline auto l_live(lua_State* L) -> int
    {
        Snapshot s{};
        if (!g_authority.read_view(s))
        {
            lua_pushboolean(L, 0);
            lua_pushnumber(L, HUGE_VAL);
            return 2;
        }
        double age = age_seconds(s, g_authority.clock());
        lua_pushboolean(L, age < LIVE_WINDOW ? 1 : 0);
        lua_pushnumber(L, age);
        return 2;
    }

    // Smoothwalker.enabled() -> bool: the processor's live switch (Smoothwalker's O, the ini, the Mod Menu); false
    // with no processor registered.
    inline auto l_enabled(lua_State* L) -> int
    {
        lua_pushboolean(L, g_authority.enabled() ? 1 : 0);
        return 1;
    }

    // A finite number field of the table at index, or `fallback` when absent. Sets *bad on a non-number or non-finite value.
    inline auto num_field(lua_State* L, int index, const char* key, double fallback, bool* bad) -> double
    {
        lua_getfield(L, index, key);
        double out = fallback;
        if (!lua_isnil(L, -1))
        {
            if (lua_type(L, -1) != LUA_TNUMBER) *bad = true;
            else
            {
                out = lua_tonumber(L, -1);
                if (!std::isfinite(out)) *bad = true;
            }
        }
        lua_pop(L, 1);
        return out;
    }

    // A sub-table {x,y,z} or {pitch,yaw,roll}; absent means zero.
    inline auto vec_field(lua_State* L, int index, const char* key, const char* const names[3], double out[3], bool* bad) -> void
    {
        lua_getfield(L, index, key);
        if (lua_istable(L, -1))
        {
            for (int k = 0; k < 3; ++k) out[k] = num_field(L, lua_gettop(L), names[k], 0.0, bad);
        }
        else if (!lua_isnil(L, -1)) *bad = true;
        lua_pop(L, 1);
    }

    // Smoothwalker.layer_set{ offset = {x,y,z}, rotation = {pitch,yaw,roll}, fov = delta, fov_abs = deg, weight = 0..1,
    //                         blend = s, ttl = s } -> true | nil, reason
    // Replaces the caller's layer. ttl is a lease: past it the layer fades out as if cleared; refresh by calling again.
    // Authority::layer_set clamps weight, blend and fov_abs.
    inline auto l_layer_set(lua_State* L) -> int
    {
        if (auto why = wrong_thread()) return fail(L, why);
        if (!lua_istable(L, 1)) return fail(L, "table_expected");
        bool bad = false;
        Layer l;
        static const char* const xyz[3] = {"x", "y", "z"};
        static const char* const pyr[3] = {"pitch", "yaw", "roll"};
        vec_field(L, 1, "offset", xyz, l.offset, &bad);
        vec_field(L, 1, "rotation", pyr, l.rotation, &bad);
        l.fov_delta = num_field(L, 1, "fov", 0.0, &bad);
        l.fov_abs = num_field(L, 1, "fov_abs", NAN, &bad);
        l.weight = num_field(L, 1, "weight", 1.0, &bad);
        l.blend = num_field(L, 1, "blend", 0.0, &bad);
        double ttl = num_field(L, 1, "ttl", 0.0, &bad);
        if (bad) return fail(L, "not_finite");
        lua_State* me = main_state(L);
        if (auto why = reason_of(g_authority.layer_set(me, l, ttl))) return fail(L, why);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Smoothwalker.layer_clear() -> true | nil, reason
    inline auto l_layer_clear(lua_State* L) -> int
    {
        if (auto why = wrong_thread()) return fail(L, why);
        lua_State* me = main_state(L);
        if (auto why = reason_of(g_authority.layer_clear(me))) return fail(L, why);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Smoothwalker.layers() -> { {mod = name, active = bool, fov = delta, fov_abs = deg|nil, weight = w}, ... } for debugging.
    inline auto l_layers(lua_State* L) -> int
    {
        Layer layers[MAX_LAYERS];
        std::string owners[MAX_LAYERS];
        g_authority.layers(layers, owners);
        lua_newtable(L);
        int n = 0;
        for (int i = 0; i < MAX_LAYERS; ++i)
        {
            if (owners[i].empty()) continue;
            lua_createtable(L, 0, 5);
            lua_pushstring(L, owners[i].c_str()); lua_setfield(L, -2, "mod");
            lua_pushboolean(L, layers[i].active ? 1 : 0); lua_setfield(L, -2, "active");
            lua_pushnumber(L, layers[i].fov_delta); lua_setfield(L, -2, "fov");
            if (std::isfinite(layers[i].fov_abs)) { lua_pushnumber(L, layers[i].fov_abs); lua_setfield(L, -2, "fov_abs"); }
            lua_pushnumber(L, layers[i].weight); lua_setfield(L, -2, "weight");
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }

    // Smoothwalker.claim{ keep_layers = bool, ttl = s } -> true | nil, reason
    // The argument table is optional; keep_layers defaults to false and ttl to no lease. First come, no priorities.
    // The owner calling again gets nil, "already_yours"; with a ttl that call also renews its lease (Authority::claim).
    inline auto l_claim(lua_State* L) -> int
    {
        if (auto why = wrong_thread()) return fail(L, why);
        bool keep = false, bad = false;
        double ttl = 0.0;
        if (lua_istable(L, 1))
        {
            lua_getfield(L, 1, "keep_layers");
            keep = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            ttl = num_field(L, 1, "ttl", 0.0, &bad);
        }
        if (bad) return fail(L, "not_finite");
        lua_State* me = main_state(L);
        if (auto why = reason_of(g_authority.claim(me, keep, ttl))) return fail(L, why);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Smoothwalker.release("cut" | "glide") -> true | nil, reason. "cut" when the mode is absent.
    inline auto l_release(lua_State* L) -> int
    {
        if (auto why = wrong_thread()) return fail(L, why);
        bool glide = false;
        if (!lua_isnoneornil(L, 1))
        {
            if (lua_type(L, 1) != LUA_TSTRING) return fail(L, "bad_mode");
            const char* mode = lua_tostring(L, 1);
            if (std::strcmp(mode, "glide") == 0) glide = true;
            else if (std::strcmp(mode, "cut") != 0) return fail(L, "bad_mode");
        }
        lua_State* me = main_state(L);
        if (auto why = reason_of(g_authority.release(me, glide))) return fail(L, why);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Smoothwalker.owner() -> the owning mod's name, or nil. Any thread: an expired lease reads as nobody here, and
    // the drop itself is left to claim/release on the game thread (Authority::owner).
    inline auto l_owner(lua_State* L) -> int
    {
        std::string mod = g_authority.owner(nullptr);
        if (mod.empty()) lua_pushnil(L);
        else lua_pushstring(L, mod.c_str());
        return 1;
    }

    inline constexpr const char* FUNCTION_NAMES[] = {"register", "view",  "live",  "enabled", "layer_set",
                                                     "layer_clear", "layers", "claim", "release", "owner"};

    // on_lua_start: the consumer's bookkeeping (Authority::install, a re-key included), then a fresh table.
    inline auto install(lua_State* L, std::string mod, std::string mod_version) -> void
    {
        lua_State* main = main_state(L);
        std::string version = g_authority.install(main, std::move(mod), std::move(mod_version));
        lua_createtable(L, 0, 13);
        lua_pushinteger(L, API_VERSION); lua_setfield(L, -2, "api_version");
        lua_pushstring(L, version.c_str()); lua_setfield(L, -2, "mod_version");
        lua_pushcfunction(L, l_register); lua_setfield(L, -2, "register");
        lua_pushcfunction(L, l_view); lua_setfield(L, -2, "view");
        lua_pushcfunction(L, l_live); lua_setfield(L, -2, "live");
        lua_pushcfunction(L, l_enabled); lua_setfield(L, -2, "enabled");
        lua_pushcfunction(L, l_layer_set); lua_setfield(L, -2, "layer_set");
        lua_pushcfunction(L, l_layer_clear); lua_setfield(L, -2, "layer_clear");
        lua_pushcfunction(L, l_layers); lua_setfield(L, -2, "layers");
        lua_pushcfunction(L, l_claim); lua_setfield(L, -2, "claim");
        lua_pushcfunction(L, l_release); lua_setfield(L, -2, "release");
        lua_pushcfunction(L, l_owner); lua_setfield(L, -2, "owner");
        lua_setglobal(L, "Smoothwalker");
    }

    // A consumer may hold `local SW = Smoothwalker`, so the table is kept and every function in it is replaced
    // by a pure-Lua stub answering nil, "unloaded": no pointer into this DLL survives an unload or hot reload.
    // The consumer's layer fades out and its slot is freed; a consumer that still owns the camera releases with a
    // cut (Authority::uninstall).
    inline auto uninstall(lua_State* L) -> void
    {
        g_authority.uninstall(main_state(L));
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
        for (const char* name : FUNCTION_NAMES)
        {
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, name);
        }
        lua_pop(L, 1); // stub
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "api_version");
        lua_pop(L, 1); // table
    }

    // Every state still holding a live table: the unload path. The Authority's keys are the states' main lua_State*.
    inline auto uninstall_all() -> void
    {
        for (const void* key : g_authority.consumers()) uninstall(const_cast<lua_State*>(static_cast<const lua_State*>(key)));
    }
} // namespace dw::camera::lua
