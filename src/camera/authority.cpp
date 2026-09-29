// The camera API's state: consumers, the owner, layers (camera/authority.hpp). The qpc_now() calls are part of the
// contract: the equivalence harness counts them, in order.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "authority.hpp"
#include "frame.hpp"
#include "../common/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace dw::camera
{
    Authority g_authority;

    auto Authority::link(bool (*enabled)(), std::atomic<bool>* reset, std::atomic<int>* reset_reason, const ViewSnapshot* snapshot) -> void
    {
        m_enabled = enabled;
        m_reset = reset;
        m_reset_reason = reset_reason;
        m_snapshot = snapshot;
    }

    auto Authority::owner(int64_t* expires_out) const -> std::string
    {
        std::string mod;
        int64_t expires = 0;
        {
            std::lock_guard guard(m_mutex);
            expires = m_owner_expires.load(std::memory_order_relaxed);
            if (m_owner_state && (expires == 0 || qpc_now() < expires)) mod = m_owner_mod;
        }
        if (expires_out) *expires_out = expires;
        return mod;
    }

    auto Authority::layers(Layer (&out)[MAX_LAYERS], std::string (&owners)[MAX_LAYERS]) const -> void
    {
        AcquireSRWLockShared(&m_layers_lock);
        std::memcpy(out, m_layers, sizeof(out));
        ReleaseSRWLockShared(&m_layers_lock);
        std::lock_guard guard(m_mutex);
        for (int i = 0; i < MAX_LAYERS; ++i) owners[i] = m_slot_owner[i];
    }

    auto Authority::consumers() const -> std::vector<const void*>
    {
        std::vector<const void*> keys;
        std::lock_guard guard(m_mutex);
        for (auto& [key, c] : m_states) keys.push_back(key);
        return keys;
    }

    // Under m_mutex. Drops the claim and tells the hook how to come back: a cut snaps on the next update (the reset
    // flag, the same one a teleport sets), a glide crossfades over position_transition from the view the owner left
    // on screen, because out_offset / out_rotation / out_fov tracked the game's view while owned.
    auto Authority::release_locked(bool glide) -> void
    {
        m_owner_state = nullptr;
        m_owner_mod.clear();
        // How to come back goes out first: the hook's acquire-load of the slot then implies this is visible, so an
        // update that sees the claim dropped always sees the glide too and never takes the falling edge's cut.
        if (glide) m_release_generation.fetch_add(1, std::memory_order_relaxed);
        else if (m_reset)
        {
            if (m_reset_reason) m_reset_reason->store(static_cast<int>(Snap::ApiCut), std::memory_order_relaxed);
            m_reset->store(true, std::memory_order_relaxed);
        }
        m_owner_keep_layers.store(false, std::memory_order_relaxed);
        m_owner_expires.store(0, std::memory_order_relaxed);
        m_owner_slot.store(-1, std::memory_order_release);
    }

    // Under m_mutex. A lease that ran out is nobody's claim: the identity is dropped here the first time the game
    // thread notices, which is a release("cut"), the same thing the hook already stopped doing on its own clock.
    auto Authority::drop_expired_locked() -> void
    {
        if (!m_owner_state) return;
        auto expires = m_owner_expires.load(std::memory_order_relaxed);
        if (expires != 0 && qpc_now() >= expires) release_locked(false);
    }

    // Under m_mutex. Fades the slot out over its last blend; the slot stays the consumer's.
    auto Authority::clear_slot_locked(int slot) -> void
    {
        if (slot < 0) return;
        AcquireSRWLockExclusive(&m_layers_lock);
        m_layers[slot].active = false;
        m_layers[slot].generation = ++m_layer_generation;
        ReleaseSRWLockExclusive(&m_layers_lock);
        m_layers_any.store(true, std::memory_order_relaxed); // the hook runs the fade and idles the flag
    }

    auto Authority::install(const void* key, std::string mod, std::string mod_version) -> std::string
    {
        std::lock_guard guard(m_mutex);
        if (m_owner_state == key) release_locked(false);
        if (auto it = m_states.find(key); it != m_states.end() && it->second.slot >= 0)
        {
            clear_slot_locked(it->second.slot);
            m_slot_owner[it->second.slot].clear();
        }
        m_mod_version = std::move(mod_version);
        m_states[key] = Consumer{std::move(mod), false, -1};
        return m_mod_version;
    }

    auto Authority::uninstall(const void* key) -> void
    {
        std::lock_guard guard(m_mutex);
        if (m_owner_state == key) release_locked(false);
        if (auto it = m_states.find(key); it != m_states.end())
        {
            if (it->second.slot >= 0)
            {
                clear_slot_locked(it->second.slot);
                m_slot_owner[it->second.slot].clear();
            }
            m_states.erase(it);
        }
    }

    auto Authority::register_consumer(const void* key, std::string& mod, bool& first) -> ApiResult
    {
        std::lock_guard guard(m_mutex);
        auto it = m_states.find(key);
        if (it == m_states.end()) return ApiResult::UnknownState;
        first = !it->second.registered;
        it->second.registered = true;
        mod = it->second.mod;
        return ApiResult::Ok;
    }

    auto Authority::claim(const void* key, bool keep_layers, double ttl) -> ApiResult
    {
        const int64_t expires = ttl > 0 ? qpc_now() + static_cast<int64_t>(ttl * qpc_frequency()) : 0;
        // Smoothwalker's own crossfade is mid-flight: taking the camera now would strand it. The flag is only
        // trusted while the camera is actually updating, so a pause or a cutscene mid-fade cannot pin it true.
        bool fade_live = false;
        if (m_blending.load(std::memory_order_relaxed))
        {
            Snapshot s{};
            fade_live = read_view(s) && age_seconds(s) < LIVE_WINDOW;
        }
        std::lock_guard guard(m_mutex);
        drop_expired_locked();
        auto it = m_states.find(key);
        if (it == m_states.end()) return ApiResult::UnknownState;
        if (m_owner_state == key)
        {
            // A repeat claim that carries a ttl renews the claim, the way a layer_set refreshes a layer: the lease
            // restarts from now and keep_layers takes this call's value. Without a ttl nothing changes, so a bare
            // claim() used as a probe cannot strip a lease. Either way the answer is AlreadyYours.
            if (expires != 0)
            {
                m_owner_keep_layers.store(keep_layers, std::memory_order_relaxed);
                m_owner_expires.store(expires, std::memory_order_relaxed);
            }
            return ApiResult::AlreadyYours;
        }
        if (m_owner_state) return ApiResult::Taken;
        if (fade_live) return ApiResult::MustKeep;
        m_owner_mod = it->second.mod;
        m_owner_state = key;
        m_owner_keep_layers.store(keep_layers, std::memory_order_relaxed);
        m_owner_expires.store(expires, std::memory_order_relaxed);
        m_owner_slot.store(0, std::memory_order_release);
        return ApiResult::Ok;
    }

    auto Authority::release(const void* key, bool glide) -> ApiResult
    {
        std::lock_guard guard(m_mutex);
        drop_expired_locked(); // an expired owner answers NotOwner, like anyone else who does not hold it
        if (m_states.find(key) == m_states.end()) return ApiResult::UnknownState;
        if (m_owner_state != key) return ApiResult::NotOwner;
        release_locked(glide);
        return ApiResult::Ok;
    }

    auto Authority::layer_set(const void* key, Layer l, double ttl) -> ApiResult
    {
        l.weight = std::clamp(l.weight, 0.0, 1.0);
        l.blend = std::max(l.blend, 0.0);
        if (std::isfinite(l.fov_abs)) l.fov_abs = std::clamp(l.fov_abs, 5.0, 170.0);
        l.active = true;
        l.expires = ttl > 0 ? qpc_now() + static_cast<int64_t>(ttl * qpc_frequency()) : 0;

        std::lock_guard guard(m_mutex);
        auto it = m_states.find(key);
        if (it == m_states.end()) return ApiResult::UnknownState;
        Consumer& c = it->second;
        if (c.slot < 0)
        {
            for (int i = 0; i < MAX_LAYERS && c.slot < 0; ++i)
                if (m_slot_owner[i].empty()) c.slot = i;
            if (c.slot >= 0) m_slot_owner[c.slot] = c.mod;
        }
        if (c.slot < 0) return ApiResult::NoSlot;
        AcquireSRWLockExclusive(&m_layers_lock);
        l.generation = ++m_layer_generation;
        m_layers[c.slot] = l;
        ReleaseSRWLockExclusive(&m_layers_lock);
        m_layers_any.store(true, std::memory_order_relaxed);
        return ApiResult::Ok;
    }

    auto Authority::layer_clear(const void* key) -> ApiResult
    {
        std::lock_guard guard(m_mutex);
        auto it = m_states.find(key);
        if (it == m_states.end()) return ApiResult::UnknownState;
        clear_slot_locked(it->second.slot);
        return ApiResult::Ok;
    }

    auto Authority::apply_layers(double* location, double* rotation, float& fov, double dt) -> bool
    {
        if (!m_layers_any.load(std::memory_order_relaxed)) return false;
        Layer layers[MAX_LAYERS];
        AcquireSRWLockShared(&m_layers_lock);
        std::memcpy(layers, m_layers, sizeof(layers));
        ReleaseSRWLockShared(&m_layers_lock);

        const int64_t now = qpc_now();
        bool any = false, touched = false;
        double sum[7]{};
        for (int i = 0; i < MAX_LAYERS; ++i)
        {
            const Layer& l = layers[i];
            LayerState& st = m_layer_state[i];
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
            m_layers_any.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!touched) return true;

        dw::Quat q = dw::from_rotator(rotation[0], rotation[1], rotation[2]);
        dw::Vec3 moved = dw::rotate(q, dw::Vec3{sum[0], sum[1], sum[2]});
        location[0] += moved.x;
        location[1] += moved.y;
        location[2] += moved.z;
        for (int k = 0; k < 3; ++k) rotation[k] += sum[3 + k];
        fov = std::clamp(fov + static_cast<float>(sum[6]), 5.0f, 170.0f);
        return true;
    }

    auto Authority::reset() -> void
    {
        {
            std::lock_guard guard(m_mutex);
            m_states.clear();
            for (auto& owner : m_slot_owner) owner.clear();
            m_mod_version.clear();
            m_owner_state = nullptr;
            m_owner_mod.clear();
            m_owner_keep_layers.store(false);
            m_owner_expires.store(0);
            m_release_generation.store(0);
            m_owner_slot.store(-1);
        }
        AcquireSRWLockExclusive(&m_layers_lock);
        for (auto& layer : m_layers) layer = Layer{};
        m_layer_generation = 0;
        ReleaseSRWLockExclusive(&m_layers_lock);
        for (auto& state : m_layer_state) state = LayerState{};
        m_layers_any.store(false);
        m_blending.store(false);
        m_game_thread.store(0);
    }

    auto Authority::inspect() const -> Inspect
    {
        Inspect out{};
        out.blending = m_blending.load();
        out.layers_any = m_layers_any.load();
        out.owner_slot = m_owner_slot.load();
        out.owner_expires = m_owner_expires.load();
        out.owner_keep_layers = m_owner_keep_layers.load();
        out.release_generation = m_release_generation.load();
        out.layer_generation = m_layer_generation;
        std::memcpy(out.layers, m_layers, sizeof(out.layers));
        std::memcpy(out.layer_state, m_layer_state, sizeof(out.layer_state));
        std::lock_guard guard(m_mutex);
        out.owner_state = m_owner_state;
        out.owner_mod = m_owner_mod;
        out.consumers = m_states.size();
        return out;
    }
} // namespace dw::camera
