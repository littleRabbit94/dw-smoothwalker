// The Smoothwalker table handed to every Lua mod (docs/design.md, "Other camera mods and a camera API").
//
// Slice 1: register(), version fields, view(), live(), enabled(). These read atomics or a seqlock snapshot the
// hook publishes once per player-camera update, so they are safe from any thread: a mod may call them at its
// top level (UE4SS mod thread), from a key bind or hook (game thread), or from LoopAsync.
// Slice 2: layer_set(), layer_clear(), layers(). One override layer per consumer, applied in the hook after the
// follow and the wall clamp. These write state the hook reads and are game thread only ("bad_thread").
// Slice 3: claim(), release(mode), owner(). One camera owner at a time. While a claim is held the follow math
// keeps running on the game's view and the hook writes nothing, so a release can cut or glide back. A claim may
// take a ttl lease, so a consumer that crashes mid-claim cannot hold the camera forever. claim() and release()
// are game thread only; owner() reads under g_mutex and is thread-agnostic.
//
// Included from dllmain.cpp after the UE4SS headers: needs the Windows types they bring in.
#pragma once

#include "smoothing.hpp"

#include <algorithm>
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
    constexpr int API_VERSION = 3;

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
    inline std::atomic<bool>* g_reset = nullptr;   // the hook's hard-cut flag, set by dllmain: release("cut")
    inline std::atomic<uint32_t> g_game_thread{0};  // captured on the engine tick

    // ---------------------------------------------------------------------------------------------- authority
    //
    // One owner at a time. The owner's identity (its main lua_State*, its mod name) lives on the game-thread side
    // under g_mutex; the hook only ever sees the numbers below (g_owner_slot, g_owner_keep_layers and
    // g_owner_expires, g_release_generation, and g_blending which it writes), never a string or a map.

    // The owner's index in the owner table, -1 when nobody owns. The table is one entry wide (one owner at a time,
    // no priorities), so the published value is 0 or -1.
    inline std::atomic<int> g_owner_slot{-1};
    inline std::atomic<bool> g_owner_keep_layers{false}; // the owner asked for layers to keep being applied
    inline std::atomic<int64_t> g_owner_expires{0};      // QPC the claim's lease runs out at; 0: no lease
    inline std::atomic<uint64_t> g_release_generation{0}; // bumped by release("glide"); folded into the hook's `changed`
    inline std::atomic<bool> g_blending{false};           // published by the hook: its crossfade is mid-flight

    inline auto qpc_now() -> int64_t
    {
        LARGE_INTEGER li{};
        QueryPerformanceCounter(&li);
        return li.QuadPart;
    }

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
        s.qpc = qpc_now();
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
        return static_cast<double>(qpc_now() - s.qpc) / qpc_frequency();
    }

    constexpr double LIVE_WINDOW = 0.25; // s, the camera_live() window

    // ---------------------------------------------------------------------------------------------- layers

    constexpr int MAX_LAYERS = 8;

    // What a consumer asked for. Numbers only: the hook copies the array under a shared lock every update.
    struct Layer
    {
        bool active = false;
        double offset[3]{};     // camera frame: x forward, y right, z up, cm
        double rotation[3]{};   // pitch, yaw, roll added, degrees
        double fov_delta = 0;   // degrees added
        double fov_abs = NAN;   // absolute FOV, blended in by weight; NAN: none
        double weight = 1;      // 0..1, scales everything above
        double blend = 0;       // s to ease a change in or out; 0 snaps
        int64_t expires = 0;    // QPC; 0 never. Past it the layer fades out as if cleared.
        uint64_t generation = 0;
    };

    inline SRWLOCK g_layers_lock = SRWLOCK_INIT;
    inline Layer g_layers[MAX_LAYERS]{};
    inline uint64_t g_layer_generation = 0;      // under g_layers_lock
    inline std::atomic<bool> g_layers_any{false}; // set by a layer_set, cleared by the hook once every slot is idle

    // Hook-thread state per slot: the seven applied numbers eased from `from` to the target.
    struct LayerState
    {
        double cur[7]{};
        double from[7]{};
        double elapsed = 0, duration = 0;
        uint64_t seen = 0;
        bool was_active = false;
        double last_blend = 0;
        bool idle = true;
    };
    inline LayerState g_layer_state[MAX_LAYERS]{};

    // The hook: adds every layer to the view about to be written. dt is the world delta of this update.
    // Returns true when a layer changed the view or is still fading, so the caller writes the view back.
    inline auto apply_layers(double* location, double* rotation, float& fov, double dt) -> bool
    {
        if (!g_layers_any.load(std::memory_order_relaxed)) return false;
        Layer layers[MAX_LAYERS];
        AcquireSRWLockShared(&g_layers_lock);
        std::memcpy(layers, g_layers, sizeof(layers));
        ReleaseSRWLockShared(&g_layers_lock);

        const int64_t now = qpc_now();
        bool any = false, touched = false;
        double sum[7]{};
        for (int i = 0; i < MAX_LAYERS; ++i)
        {
            const Layer& l = layers[i];
            LayerState& st = g_layer_state[i];
            bool live = l.active && (l.expires == 0 || now < l.expires);
            double target[7]{};
            if (live)
            {
                double w = std::clamp(l.weight, 0.0, 1.0);
                for (int k = 0; k < 3; ++k) target[k] = l.offset[k] * w;
                for (int k = 0; k < 3; ++k) target[3 + k] = l.rotation[k] * w;
                target[6] = l.fov_delta * w + (std::isfinite(l.fov_abs) ? (l.fov_abs - static_cast<double>(fov)) * w : 0.0);
                st.last_blend = std::max(l.blend, 0.0);
            }
            if (l.generation != st.seen || live != st.was_active)
            {
                st.seen = l.generation;
                st.was_active = live;
                std::memcpy(st.from, st.cur, sizeof(st.cur));
                st.elapsed = 0;
                st.duration = st.last_blend;
            }
            st.elapsed += std::clamp(dt, 0.0, 0.1);
            double s = st.duration > 0 ? std::min(st.elapsed / st.duration, 1.0) : 1.0;
            double w = s * s * (3.0 - 2.0 * s);
            bool nonzero = false;
            for (int k = 0; k < 7; ++k)
            {
                st.cur[k] = st.from[k] + (target[k] - st.from[k]) * w;
                if (!std::isfinite(st.cur[k])) st.cur[k] = 0.0;
                if (std::abs(st.cur[k]) > 1e-6) nonzero = true;
                sum[k] += st.cur[k];
            }
            st.idle = !live && !nonzero;
            if (!st.idle) any = true;
            if (nonzero) touched = true;
        }
        if (!any)
        {
            g_layers_any.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!touched) return true;

        dwsc::Quat q = dwsc::from_rotator(rotation[0], rotation[1], rotation[2]);
        dwsc::Vec3 moved = dwsc::rotate(q, dwsc::Vec3{sum[0], sum[1], sum[2]});
        location[0] += moved.x;
        location[1] += moved.y;
        location[2] += moved.z;
        for (int k = 0; k < 3; ++k) rotation[k] += sum[3 + k];
        fov = std::clamp(fov + static_cast<float>(sum[6]), 5.0f, 170.0f);
        return true;
    }

    // ---------------------------------------------------------------------------------------------- Lua side

    struct Consumer
    {
        std::string mod;     // the Lua mod's folder name, from on_lua_start
        bool registered = false;
        int slot = -1;       // its layer, once it set one
    };

    inline std::mutex g_mutex;
    inline std::unordered_map<lua_State*, Consumer> g_states; // keyed by each mod's main state
    inline std::string g_slot_owner[MAX_LAYERS];               // under g_mutex
    inline std::string g_mod_version;
    inline lua_State* g_owner_state = nullptr; // the camera owner's main state, under g_mutex; nullptr: nobody
    inline std::string g_owner_mod;            // its mod name, under g_mutex

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

    inline auto fail(lua_State* L, const char* reason) -> int
    {
        lua_pushnil(L);
        lua_pushstring(L, reason);
        return 2;
    }

    // nullptr when the caller is on the game thread, else the reason to answer.
    inline auto wrong_thread() -> const char*
    {
        auto game = g_game_thread.load(std::memory_order_relaxed);
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
    // Every Lua-facing function does its bookkeeping under g_mutex into locals and releases the lock before any
    // lua_* call: a Lua error longjmps out of a C function, and one thrown under the lock would leave it held
    // and deadlock every later API call.
    inline auto l_register(lua_State* L) -> int
    {
        std::string mod;
        const char* reason = nullptr;
        {
            std::lock_guard guard(g_mutex);
            auto* c = consumer_of(L);
            if (!c) reason = "unknown_state";
            else
            {
                if (!c->registered)
                {
                    c->registered = true;
                    RC::Output::send<RC::LogLevel::Normal>(STR("[DWSmoothwalker] API consumer '{}' registered\n"), RC::to_wstring(c->mod));
                }
                mod = c->mod;
            }
        }
        if (reason) return fail(L, reason);
        lua_pushstring(L, mod.c_str());
        return 1;
    }

    // Smoothwalker.view() -> { game = {x,y,z,pitch,yaw,roll,fov}, shown = {...}, pivot = {x,y,z} | nil, age = s }
    // nil, "no_view" before the first player-camera update.
    inline auto l_view(lua_State* L) -> int
    {
        Snapshot s{};
        if (!read(s)) return fail(L, "no_view");
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
        l.weight = std::clamp(num_field(L, 1, "weight", 1.0, &bad), 0.0, 1.0);
        l.blend = std::max(num_field(L, 1, "blend", 0.0, &bad), 0.0);
        double ttl = num_field(L, 1, "ttl", 0.0, &bad);
        if (bad) return fail(L, "not_finite");
        if (std::isfinite(l.fov_abs)) l.fov_abs = std::clamp(l.fov_abs, 5.0, 170.0);
        l.active = true;
        l.expires = ttl > 0 ? qpc_now() + static_cast<int64_t>(ttl * qpc_frequency()) : 0;

        lua_State* me = main_state(L);
        const char* reason = nullptr;
        {
            std::lock_guard guard(g_mutex);
            auto it = g_states.find(me);
            if (it == g_states.end()) reason = "unknown_state";
            else
            {
                Consumer& c = it->second;
                if (c.slot < 0)
                {
                    for (int i = 0; i < MAX_LAYERS && c.slot < 0; ++i)
                        if (g_slot_owner[i].empty()) c.slot = i;
                    if (c.slot >= 0) g_slot_owner[c.slot] = c.mod;
                }
                if (c.slot < 0) reason = "no_slot";
                else
                {
                    AcquireSRWLockExclusive(&g_layers_lock);
                    l.generation = ++g_layer_generation;
                    g_layers[c.slot] = l;
                    ReleaseSRWLockExclusive(&g_layers_lock);
                    g_layers_any.store(true, std::memory_order_relaxed);
                }
            }
        }
        if (reason) return fail(L, reason);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Under g_mutex. Fades the slot out over its last blend; the slot stays the consumer's.
    inline auto clear_slot(int slot) -> void
    {
        if (slot < 0) return;
        AcquireSRWLockExclusive(&g_layers_lock);
        g_layers[slot].active = false;
        g_layers[slot].generation = ++g_layer_generation;
        ReleaseSRWLockExclusive(&g_layers_lock);
        g_layers_any.store(true, std::memory_order_relaxed); // the hook runs the fade and idles the flag
    }

    // Smoothwalker.layer_clear() -> true | nil, reason
    inline auto l_layer_clear(lua_State* L) -> int
    {
        if (auto why = wrong_thread()) return fail(L, why);
        lua_State* me = main_state(L);
        const char* reason = nullptr;
        {
            std::lock_guard guard(g_mutex);
            auto it = g_states.find(me);
            if (it == g_states.end()) reason = "unknown_state";
            else clear_slot(it->second.slot);
        }
        if (reason) return fail(L, reason);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Smoothwalker.layers() -> { {mod = name, active = bool, fov = delta, fov_abs = deg|nil, weight = w}, ... } for debugging.
    inline auto l_layers(lua_State* L) -> int
    {
        Layer layers[MAX_LAYERS];
        AcquireSRWLockShared(&g_layers_lock);
        std::memcpy(layers, g_layers, sizeof(layers));
        ReleaseSRWLockShared(&g_layers_lock);
        std::string owners[MAX_LAYERS];
        {
            std::lock_guard guard(g_mutex);
            for (int i = 0; i < MAX_LAYERS; ++i) owners[i] = g_slot_owner[i];
        }
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

    // ------------------------------------------------------------------------------------- authority, Lua side

    // Under g_mutex. Drops the claim and tells the hook how to come back: a cut snaps on the next update
    // (g_reset, the same flag a teleport sets), a glide crossfades over position_transition from the view the
    // owner left on screen, because out_offset / out_rotation / out_fov tracked the game's view while owned.
    inline auto release_locked(bool glide) -> void
    {
        g_owner_state = nullptr;
        g_owner_mod.clear();
        g_owner_slot.store(-1, std::memory_order_release);
        g_owner_keep_layers.store(false, std::memory_order_relaxed);
        g_owner_expires.store(0, std::memory_order_relaxed);
        if (glide) g_release_generation.fetch_add(1, std::memory_order_relaxed);
        else if (g_reset) g_reset->store(true, std::memory_order_relaxed);
    }

    // Under g_mutex. A lease that ran out is nobody's claim: the identity is dropped here the first time the game
    // thread notices, which is a release("cut"), the same thing the hook already stopped doing on its own clock.
    inline auto drop_expired_locked() -> void
    {
        if (!g_owner_state) return;
        auto expires = g_owner_expires.load(std::memory_order_relaxed);
        if (expires != 0 && qpc_now() >= expires) release_locked(false);
    }

    // Smoothwalker.claim{ keep_layers = bool, ttl = s } -> true | nil, reason
    // The argument table is optional; keep_layers defaults to false and ttl to no lease. First come, no priorities.
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
        const int64_t expires = ttl > 0 ? qpc_now() + static_cast<int64_t>(ttl * qpc_frequency()) : 0;
        // Smoothwalker's own crossfade is mid-flight: taking the camera now would strand it. The flag is only
        // trusted while the camera is actually updating, so a pause or a cutscene mid-fade cannot pin it true.
        bool fade_live = false;
        if (g_blending.load(std::memory_order_relaxed))
        {
            Snapshot s{};
            fade_live = read(s) && age_seconds(s) < LIVE_WINDOW;
        }
        const char* reason = nullptr;
        {
            std::lock_guard guard(g_mutex);
            drop_expired_locked();
            auto it = g_states.find(me);
            if (it == g_states.end()) reason = "unknown_state";
            else if (g_owner_state == me) reason = "already_yours";
            else if (g_owner_state) reason = "taken";
            else if (fade_live) reason = "must_keep";
            else
            {
                g_owner_mod = it->second.mod;
                g_owner_state = me;
                g_owner_keep_layers.store(keep, std::memory_order_relaxed);
                g_owner_expires.store(expires, std::memory_order_relaxed);
                g_owner_slot.store(0, std::memory_order_release);
            }
        }
        if (reason) return fail(L, reason);
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
        const char* reason = nullptr;
        {
            std::lock_guard guard(g_mutex);
            drop_expired_locked(); // an expired owner answers not_owner, like anyone else who does not hold it
            if (g_states.find(me) == g_states.end()) reason = "unknown_state";
            else if (g_owner_state != me) reason = "not_owner";
            else release_locked(glide);
        }
        if (reason) return fail(L, reason);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Smoothwalker.owner() -> the owning mod's name, or nil. Reads under g_mutex only, so any thread may call it:
    // an expired lease reads as nobody here, and the drop itself is left to claim/release on the game thread.
    inline auto l_owner(lua_State* L) -> int
    {
        std::string mod;
        {
            std::lock_guard guard(g_mutex);
            auto expires = g_owner_expires.load(std::memory_order_relaxed);
            if (g_owner_state && (expires == 0 || qpc_now() < expires)) mod = g_owner_mod;
        }
        if (mod.empty()) lua_pushnil(L);
        else lua_pushstring(L, mod.c_str());
        return 1;
    }

    inline constexpr const char* FUNCTION_NAMES[] = {"register", "view",  "live",  "enabled", "layer_set",
                                                     "layer_clear", "layers", "claim", "release", "owner"};

    inline auto install(lua_State* L, std::string mod, std::string mod_version) -> void
    {
        lua_State* main = main_state(L);
        std::string version;
        {
            std::lock_guard guard(g_mutex);
            // A Lua mod restarted without an on_lua_stop (a hot reload, a script error at load) comes back on the
            // same state: whatever it claimed before is not its claim any more, so the camera goes back to nobody.
            if (g_owner_state == main) release_locked(false);
            g_mod_version = std::move(mod_version);
            g_states[main] = Consumer{std::move(mod), false, -1};
            version = g_mod_version;
        }
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
    // The consumer's layer fades out and its slot is freed; a consumer that still owns the camera releases with a cut.
    inline auto uninstall(lua_State* L) -> void
    {
        lua_State* main = main_state(L);
        {
            std::lock_guard guard(g_mutex);
            if (g_owner_state == main) release_locked(false);
            if (auto it = g_states.find(main); it != g_states.end())
            {
                if (it->second.slot >= 0)
                {
                    clear_slot(it->second.slot);
                    g_slot_owner[it->second.slot].clear();
                }
                g_states.erase(it);
            }
        }
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
