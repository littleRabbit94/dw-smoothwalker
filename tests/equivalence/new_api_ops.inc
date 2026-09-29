// The working tree's camera API game-thread operations without Lua (new_driver.cpp includes this inside
// `using namespace dw::camera`). Each is what the matching lua_api.hpp function does after its argument parsing:
// the Authority's own method, called (install / uninstall for on_lua_start / on_lua_stop, claim, release, layer_set,
// layer_clear). Only the Lua argument checks are mirrored: a non-finite number is refused before the Authority is
// called, as num_field makes l_claim and l_layer_set do. Consumers are fake keys, the same numbers as the baseline's
// fake lua_State pointers; the harness never enters Lua.

namespace ops
{
    inline auto key_of(int consumer) -> const void*
    {
        return reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1000 + 0x100 * consumer));
    }

    inline const char* const CONSUMER_NAMES[] = {"ConsumerA", "ConsumerB"};

    inline auto code(ApiResult result) -> int
    {
        switch (result)
        {
        case ApiResult::Ok: return harness::Ok;
        case ApiResult::UnknownState: return harness::UnknownState;
        case ApiResult::AlreadyYours: return harness::AlreadyYours;
        case ApiResult::Taken: return harness::Taken;
        case ApiResult::MustKeep: return harness::MustKeep;
        case ApiResult::NotOwner: return harness::NotOwner;
        case ApiResult::NoSlot: return harness::NoSlot;
        }
        std::abort();
    }

    inline auto install(int consumer) -> void { g_authority.install(key_of(consumer), CONSUMER_NAMES[consumer], "0.10.1"); }

    inline auto uninstall(int consumer) -> void { g_authority.uninstall(key_of(consumer)); }

    inline auto claim(int consumer, bool keep, double ttl) -> int
    {
        if (!std::isfinite(ttl)) return harness::NotFinite;
        return code(g_authority.claim(key_of(consumer), keep, ttl));
    }

    inline auto release(int consumer, bool glide) -> int { return code(g_authority.release(key_of(consumer), glide)); }

    inline auto layer_set(int consumer, const harness::LayerArgs& a) -> int
    {
        bool bad = false;
        auto number = [&](double v) {
            if (!std::isfinite(v)) bad = true;
            return v;
        };
        Layer l;
        for (int k = 0; k < 3; ++k) l.offset[k] = number(a.offset[k]);
        for (int k = 0; k < 3; ++k) l.rotation[k] = number(a.rotation[k]);
        l.fov_delta = number(a.fov);
        l.fov_abs = std::isnan(a.fov_abs) ? NAN : number(a.fov_abs); // absent is NAN; present must be finite
        l.weight = number(a.weight);
        l.blend = number(a.blend);
        double ttl = number(a.ttl);
        if (bad) return harness::NotFinite;
        return code(g_authority.layer_set(key_of(consumer), l, ttl));
    }

    inline auto layer_clear(int consumer) -> int { return code(g_authority.layer_clear(key_of(consumer))); }

    // Everything the API side holds that the hook reads or writes, in base_api_ops.inc's record order.
    inline auto api_state(harness::Record& r) -> void
    {
        r.put("api.seq", g_api_snapshot.seq());
        const Snapshot& snapshot = g_api_snapshot.data();
        auto view = [&](const char* label, const View& v) {
            r.put(label, v.location);
            r.put(label, v.rotation);
            r.put(label, v.fov);
        };
        view("api.snapshot.game", snapshot.game);
        view("api.snapshot.shown", snapshot.shown);
        r.put("api.snapshot.pivot", snapshot.pivot);
        r.put("api.snapshot.qpc", snapshot.qpc);
        const Authority::Inspect a = g_authority.inspect();
        r.put("api.blending", a.blending);
        r.put("api.layers_any", a.layers_any);
        r.put("api.owner_slot", a.owner_slot);
        r.put("api.owner_expires", a.owner_expires);
        r.put("api.owner_keep_layers", a.owner_keep_layers);
        r.put("api.release_generation", a.release_generation);
        r.put("api.layer_generation", a.layer_generation);
        // Field by field: both structs have padding, whose bytes are not the program's.
        for (const auto& l : a.layers)
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
        for (const auto& st : a.layer_state)
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
        r.put("api.owner_state", reinterpret_cast<uintptr_t>(a.owner_state));
        r.put("api.owner_mod", a.owner_mod.data(), a.owner_mod.size());
        r.put("api.consumers", a.consumers);
    }
} // namespace ops
