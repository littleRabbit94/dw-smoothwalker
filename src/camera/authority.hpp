// The camera API's state (docs/design.md, "Other camera mods and a camera API"): API consumers, the camera owner and
// its lease, the override layers, and the hook's reads of them. No Lua and no log: the Lua binding
// (camera/lua_api.hpp) parses arguments, calls these methods and pushes their results.
//
// One owner at a time. The owner's identity (its consumer key, its mod name) lives on the game-thread side under the
// mutex; the hook only ever reads numbers (owner_slot, owner_keeps_layers and owner_expires, release_generation, and
// the blending flag it writes itself), never a string or a map. A consumer key is an opaque pointer, the consumer's
// main lua_State*. The game-thread-only methods leave the thread check to the caller.
//
// Needs the Windows types included before it.
#pragma once

#include "snapshot.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dw::camera
{
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

    // Why a call was refused; Ok when it was not. The Lua binding turns these into its reason strings.
    enum class ApiResult : int
    {
        Ok,
        UnknownState,
        AlreadyYours,
        Taken,
        MustKeep,
        NotOwner,
        NoSlot,
    };

    class Authority
    {
      public:
        struct Consumer
        {
            std::string mod;     // the Lua mod's folder name, from on_lua_start
            bool registered = false;
            int slot = -1;       // its layer, once it set one
        };

        // Read-only copy of the state, for a single-threaded check (the equivalence harness).
        struct Inspect
        {
            bool blending, layers_any;
            int owner_slot;
            int64_t owner_expires;
            bool owner_keep_layers;
            uint64_t release_generation, layer_generation;
            Layer layers[MAX_LAYERS];
            LayerState layer_state[MAX_LAYERS];
            const void* owner_state;
            std::string owner_mod;
            size_t consumers;
        };

        // The core's pipeline, set by the core at construction and kept across reset(): the processor's live switch
        // (any thread), the hook's hard-cut flag and its reason (release("cut") snaps through the same flag a
        // teleport sets), and the view snapshot claim() checks.
        auto link(bool (*enabled)(), std::atomic<bool>* reset, std::atomic<int>* reset_reason, const ViewSnapshot* snapshot) -> void;

        // ------------------------------------------------------------------------------------------ any thread

        // Smoothwalker.enabled(): the processor's live switch; false with no processor registered or no link.
        auto enabled() const -> bool { return m_enabled && m_enabled(); }
        // The view snapshot (view(), live()); false as ViewSnapshot::read, and before link().
        auto read_view(Snapshot& out) const -> bool { return m_snapshot && m_snapshot->read(out); }
        // The game thread's id, captured on the engine tick; 0 before the first.
        auto game_thread() const -> uint32_t { return m_game_thread.load(std::memory_order_relaxed); }
        auto set_game_thread(uint32_t id) -> void { m_game_thread.store(id); }
        // The owner's mod name, empty for nobody, under the mutex only: an expired lease reads as nobody here, and
        // the drop itself is left to claim and release. Writes the lease's expiry QPC (0: none) when asked.
        auto owner(int64_t* expires_out) const -> std::string;
        // The layers and their owners' names (empty: a free slot), for Smoothwalker.layers().
        auto layers(Layer (&out)[MAX_LAYERS], std::string (&owners)[MAX_LAYERS]) const -> void;
        // Every consumer key, for the unload path.
        auto consumers() const -> std::vector<const void*>;

        // --------------------------------------------------------------------------------- game thread (Lua)

        // A consumer's (re)start on `key`; returns the mod version to publish. A Lua mod restarted without an
        // on_lua_stop (a hot reload, a script error at load) comes back on the same key: whatever it claimed before
        // is not its claim any more, so the camera goes back to nobody, and the layer slot it held is freed too (its
        // layer fades out), as an uninstall does.
        auto install(const void* key, std::string mod, std::string mod_version) -> std::string;
        // The consumer's layer fades out and its slot is freed; a consumer that still owns the camera releases with
        // a cut.
        auto uninstall(const void* key) -> void;
        // Smoothwalker.register(): the consumer's mod name into `mod`; `first` true on its first registration.
        auto register_consumer(const void* key, std::string& mod, bool& first) -> ApiResult;
        // Smoothwalker.claim(): ttl finite, 0 or less for no lease. First come, no priorities. The owner calling
        // again gets AlreadyYours; with a ttl that call also renews its lease.
        auto claim(const void* key, bool keep_layers, double ttl) -> ApiResult;
        // Smoothwalker.release(): a glide crossfades back, a cut snaps.
        auto release(const void* key, bool glide) -> ApiResult;
        // Smoothwalker.layer_set(): every number of `layer` finite; replaces the consumer's layer. ttl is a lease:
        // past it the layer fades out as if cleared.
        auto layer_set(const void* key, Layer layer, double ttl) -> ApiResult;
        // Smoothwalker.layer_clear(): the consumer's layer fades out; the slot stays its.
        auto layer_clear(const void* key) -> ApiResult;

        // -------------------------------------------------------------------------------------------- the hook

        // Relaxed, except owner_slot (acquire): a release publishes how to come back before the slot, so an update
        // that sees the claim dropped always sees the glide too.
        auto owner_slot() const -> int { return m_owner_slot.load(std::memory_order_acquire); }
        auto owner_expires() const -> int64_t { return m_owner_expires.load(std::memory_order_relaxed); }
        auto owner_keeps_layers() const -> bool { return m_owner_keep_layers.load(std::memory_order_relaxed); }
        auto release_generation() const -> uint64_t { return m_release_generation.load(std::memory_order_relaxed); }
        auto layers_any() const -> bool { return m_layers_any.load(std::memory_order_relaxed); }
        // Published by the hook: its crossfade is mid-flight (claim() refuses with MustKeep while the camera is live).
        auto set_blending(bool on) -> void { m_blending.store(on, std::memory_order_relaxed); }
        // Adds every layer to the view about to be written. dt is the world delta of this update. True when a layer
        // changed the view or is still fading, so the caller writes the view back.
        auto apply_layers(double* location, double* rotation, float& fov, double dt) -> bool;

        // Every field back to its static-init value, for the next instance on the same pinned image
        // (Core::reset_globals). After every Lua table holds stubs and the hook stopped following the player. Kept:
        // the locks and the link() pointers.
        auto reset() -> void;

        auto inspect() const -> Inspect;

      private:
        auto release_locked(bool glide) -> void;
        auto drop_expired_locked() -> void;
        auto clear_slot_locked(int slot) -> void;

        bool (*m_enabled)() = nullptr;
        std::atomic<bool>* m_reset = nullptr;
        std::atomic<int>* m_reset_reason = nullptr;
        const ViewSnapshot* m_snapshot = nullptr;
        std::atomic<uint32_t> m_game_thread{0};

        // The owner's index in the owner table, -1 when nobody owns. The table is one entry wide (one owner at a
        // time, no priorities), so the published value is 0 or -1.
        std::atomic<int> m_owner_slot{-1};
        std::atomic<bool> m_owner_keep_layers{false};  // the owner asked for layers to keep being applied
        std::atomic<int64_t> m_owner_expires{0};       // QPC the claim's lease runs out at; 0: no lease
        std::atomic<uint64_t> m_release_generation{0}; // bumped by release("glide"); folded into the hook's `changed`
        std::atomic<bool> m_blending{false};           // published by the hook: its crossfade is mid-flight

        mutable SRWLOCK m_layers_lock = SRWLOCK_INIT;
        Layer m_layers[MAX_LAYERS]{};
        uint64_t m_layer_generation = 0;              // under m_layers_lock
        std::atomic<bool> m_layers_any{false};        // set by a layer_set, cleared by the hook once every slot is idle
        LayerState m_layer_state[MAX_LAYERS]{};       // the hook's

        mutable std::mutex m_mutex;
        std::unordered_map<const void*, Consumer> m_states; // keyed by each mod's main state
        std::string m_slot_owner[MAX_LAYERS];               // under m_mutex
        std::string m_mod_version;
        const void* m_owner_state = nullptr; // the camera owner's key, under m_mutex; nullptr: nobody
        std::string m_owner_mod;             // its mod name, under m_mutex
    };

    // The one instance, image-level for now: reset by Core::reset_globals, since a hot reload runs no static
    // initializer on the pinned image.
    extern Authority g_authority;
} // namespace dw::camera
