// The camera API's game-thread operations without Lua, over whichever build's dwapi namespace is in scope (the two
// drivers include this after their lua_api.hpp). Each is the bookkeeping of the matching lua_api.hpp function after
// its argument parsing, in the same order: install / uninstall (on_lua_start / on_lua_stop), l_claim, l_release,
// l_layer_set, l_layer_clear. lua_api.hpp's own functions (release_locked, drop_expired_locked, clear_slot, read,
// qpc_now, ...) are called, not copied. Consumers are fake lua_State pointers; the harness never enters Lua.

namespace ops
{
    inline auto state_of(int consumer) -> lua_State*
    {
        return reinterpret_cast<lua_State*>(static_cast<uintptr_t>(0x1000 + 0x100 * consumer));
    }

    inline const char* const CONSUMER_NAMES[] = {"ConsumerA", "ConsumerB"};

    inline auto install(int consumer) -> void
    {
        lua_State* main = state_of(consumer);
        std::lock_guard guard(dwapi::g_mutex);
#if defined(EQ_BASELINE) && !defined(EQ_BASELINE_FIXED)
        // 47f6645 as it was: no install_locked yet, and a re-key leaves the consumer's layer slot taken.
        if (dwapi::g_owner_state == main) dwapi::release_locked(false);
        dwapi::g_mod_version = "0.10.1";
        dwapi::g_states[main] = dwapi::Consumer{CONSUMER_NAMES[consumer], false, -1};
#else
        dwapi::install_locked(main, CONSUMER_NAMES[consumer], "0.10.1");
#endif
    }

    inline auto uninstall(int consumer) -> void
    {
        lua_State* main = state_of(consumer);
        std::lock_guard guard(dwapi::g_mutex);
        if (dwapi::g_owner_state == main) dwapi::release_locked(false);
        if (auto it = dwapi::g_states.find(main); it != dwapi::g_states.end())
        {
            if (it->second.slot >= 0)
            {
                dwapi::clear_slot(it->second.slot);
                dwapi::g_slot_owner[it->second.slot].clear();
            }
            dwapi::g_states.erase(it);
        }
    }

    inline auto claim(int consumer, bool keep, double ttl) -> int
    {
        if (!std::isfinite(ttl)) return harness::NotFinite;
        lua_State* me = state_of(consumer);
        const int64_t expires = ttl > 0 ? dwapi::qpc_now() + static_cast<int64_t>(ttl * dwapi::qpc_frequency()) : 0;
        bool fade_live = false;
        if (dwapi::g_blending.load(std::memory_order_relaxed))
        {
            dwapi::Snapshot s{};
            fade_live = dwapi::read(s) && dwapi::age_seconds(s) < dwapi::LIVE_WINDOW;
        }
        int reason = harness::Ok;
        {
            std::lock_guard guard(dwapi::g_mutex);
            dwapi::drop_expired_locked();
            auto it = dwapi::g_states.find(me);
            if (it == dwapi::g_states.end()) reason = harness::UnknownState;
            else if (dwapi::g_owner_state == me)
            {
                if (expires != 0)
                {
                    dwapi::g_owner_keep_layers.store(keep, std::memory_order_relaxed);
                    dwapi::g_owner_expires.store(expires, std::memory_order_relaxed);
                }
                reason = harness::AlreadyYours;
            }
            else if (dwapi::g_owner_state) reason = harness::Taken;
            else if (fade_live) reason = harness::MustKeep;
            else
            {
                dwapi::g_owner_mod = it->second.mod;
                dwapi::g_owner_state = me;
                dwapi::g_owner_keep_layers.store(keep, std::memory_order_relaxed);
                dwapi::g_owner_expires.store(expires, std::memory_order_relaxed);
                dwapi::g_owner_slot.store(0, std::memory_order_release);
            }
        }
        return reason;
    }

    inline auto release(int consumer, bool glide) -> int
    {
        lua_State* me = state_of(consumer);
        int reason = harness::Ok;
        std::lock_guard guard(dwapi::g_mutex);
        dwapi::drop_expired_locked();
        if (dwapi::g_states.find(me) == dwapi::g_states.end()) reason = harness::UnknownState;
        else if (dwapi::g_owner_state != me) reason = harness::NotOwner;
        else dwapi::release_locked(glide);
        return reason;
    }

    inline auto layer_set(int consumer, const harness::LayerArgs& a) -> int
    {
        bool bad = false;
        auto number = [&](double v) {
            if (!std::isfinite(v)) bad = true;
            return v;
        };
        dwapi::Layer l;
        for (int k = 0; k < 3; ++k) l.offset[k] = number(a.offset[k]);
        for (int k = 0; k < 3; ++k) l.rotation[k] = number(a.rotation[k]);
        l.fov_delta = number(a.fov);
        l.fov_abs = std::isnan(a.fov_abs) ? NAN : number(a.fov_abs); // absent is NAN; present must be finite
        l.weight = std::clamp(number(a.weight), 0.0, 1.0);
        l.blend = std::max(number(a.blend), 0.0);
        double ttl = number(a.ttl);
        if (bad) return harness::NotFinite;
        if (std::isfinite(l.fov_abs)) l.fov_abs = std::clamp(l.fov_abs, 5.0, 170.0);
        l.active = true;
        l.expires = ttl > 0 ? dwapi::qpc_now() + static_cast<int64_t>(ttl * dwapi::qpc_frequency()) : 0;

        lua_State* me = state_of(consumer);
        int reason = harness::Ok;
        std::lock_guard guard(dwapi::g_mutex);
        auto it = dwapi::g_states.find(me);
        if (it == dwapi::g_states.end()) reason = harness::UnknownState;
        else
        {
            dwapi::Consumer& c = it->second;
            if (c.slot < 0)
            {
                for (int i = 0; i < dwapi::MAX_LAYERS && c.slot < 0; ++i)
                    if (dwapi::g_slot_owner[i].empty()) c.slot = i;
                if (c.slot >= 0) dwapi::g_slot_owner[c.slot] = c.mod;
            }
            if (c.slot < 0) reason = harness::NoSlot;
            else
            {
                AcquireSRWLockExclusive(&dwapi::g_layers_lock);
                l.generation = ++dwapi::g_layer_generation;
                dwapi::g_layers[c.slot] = l;
                ReleaseSRWLockExclusive(&dwapi::g_layers_lock);
                dwapi::g_layers_any.store(true, std::memory_order_relaxed);
            }
        }
        return reason;
    }

    inline auto layer_clear(int consumer) -> int
    {
        lua_State* me = state_of(consumer);
        std::lock_guard guard(dwapi::g_mutex);
        auto it = dwapi::g_states.find(me);
        if (it == dwapi::g_states.end()) return harness::UnknownState;
        dwapi::clear_slot(it->second.slot);
        return harness::Ok;
    }

    // Everything the API side holds that the hook reads or writes.
    inline auto api_state(harness::Record& r) -> void
    {
        r.put("api.seq", dwapi::g_seq.load());
        auto view = [&](const char* label, const dwapi::View& v) {
            r.put(label, v.location);
            r.put(label, v.rotation);
            r.put(label, v.fov);
        };
        view("api.snapshot.game", dwapi::g_snapshot.game);
        view("api.snapshot.shown", dwapi::g_snapshot.shown);
        r.put("api.snapshot.pivot", dwapi::g_snapshot.pivot);
        r.put("api.snapshot.qpc", dwapi::g_snapshot.qpc);
        r.put("api.blending", dwapi::g_blending.load());
        r.put("api.layers_any", dwapi::g_layers_any.load());
        r.put("api.owner_slot", dwapi::g_owner_slot.load());
        r.put("api.owner_expires", dwapi::g_owner_expires.load());
        r.put("api.owner_keep_layers", dwapi::g_owner_keep_layers.load());
        r.put("api.release_generation", dwapi::g_release_generation.load());
        r.put("api.layer_generation", dwapi::g_layer_generation);
        // Field by field: both structs have padding, whose bytes are not the program's.
        for (const auto& l : dwapi::g_layers)
        {
            r.put("layer.active", l.active);
            r.put("layer.offset", l.offset);
            r.put("layer.rotation", l.rotation);
            r.put("layer.fov_delta", l.fov_delta);
            r.put("layer.fov_abs", l.fov_abs);
            r.put("layer.weight", l.weight);
            r.put("layer.blend", l.blend);
            r.put("layer.expires", l.expires);
            r.put("layer.generation", l.generation);
        }
        for (const auto& st : dwapi::g_layer_state)
        {
            r.put("layer_state.cur", st.cur);
            r.put("layer_state.from", st.from);
            r.put("layer_state.elapsed", st.elapsed);
            r.put("layer_state.duration", st.duration);
            r.put("layer_state.seen", st.seen);
            r.put("layer_state.was_active", st.was_active);
            r.put("layer_state.last_blend", st.last_blend);
            r.put("layer_state.idle", st.idle);
        }
        std::lock_guard guard(dwapi::g_mutex);
        r.put("api.owner_state", reinterpret_cast<uintptr_t>(dwapi::g_owner_state));
        r.put("api.owner_mod", dwapi::g_owner_mod.data(), dwapi::g_owner_mod.size());
        r.put("api.consumers", dwapi::g_states.size());
    }
} // namespace ops
