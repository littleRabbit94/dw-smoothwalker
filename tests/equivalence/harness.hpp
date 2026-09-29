// The equivalence harness's interface between the session driver (main.cpp) and the two builds of the hook it
// compares: the baseline (base_driver.cpp, the hook region of commit f22aa8e) and the split (new_driver.cpp,
// camera/pipeline.hpp with Smoothwalker's processor). Each driver turns the same session events into calls on its
// own build and writes what that build then holds into a Record; main.cpp compares the records byte for byte.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace harness
{
    // The settings the hook reads, by smoothwalker.ini key.
    struct HSettings
    {
        double follow_rate_h = 6.5, follow_rate_v = 10.0;
        int curve_h = 2, curve_v = 0;
        double catchup_distance = 150.0, min_rate_scale = 0.35, max_lag_h = 85.0, max_lag_v = 50.0;
        bool soft_leash = true;
        double aiming_follow = 30.0, combat_follow = 100.0, traversal_follow = 100.0;
        bool rotation_smoothing = false;
        double rotation_rate = 20.0, combat_rotation = 100.0, traversal_rotation = 100.0;
        bool wall_clamp = true;
        double reset_distance = 500.0, reset_gap = 0.25, position_transition = 0.5;
        bool log_stats = false, log_verbose = false;
    };

    // Settings for either build's dwsc::Settings (the same fields in both).
    template <typename S>
    auto to_settings(const HSettings& h) -> S
    {
        S s{};
        s.follow_rate_h = h.follow_rate_h;
        s.follow_rate_v = h.follow_rate_v;
        s.curve_h = h.curve_h;
        s.curve_v = h.curve_v;
        s.catchup_distance = h.catchup_distance;
        s.min_rate_scale = h.min_rate_scale;
        s.max_lag_h = h.max_lag_h;
        s.max_lag_v = h.max_lag_v;
        s.soft_leash = h.soft_leash;
        s.aiming_follow = h.aiming_follow;
        s.combat_follow = h.combat_follow;
        s.traversal_follow = h.traversal_follow;
        s.rotation_smoothing = h.rotation_smoothing;
        s.rotation_rate = h.rotation_rate;
        s.combat_rotation = h.combat_rotation;
        s.traversal_rotation = h.traversal_rotation;
        s.wall_clamp = h.wall_clamp;
        s.reset_distance = h.reset_distance;
        s.reset_gap = h.reset_gap;
        s.position_transition = h.position_transition;
        s.log_stats = h.log_stats;
        s.log_verbose = h.log_verbose;
        return s;
    }

    struct LayerArgs
    {
        double offset[3]{}, rotation[3]{};
        double fov = 0.0, fov_abs = NAN, weight = 1.0, blend = 0.0, ttl = 0.0;
    };

    // Bytes in the order they were put; a label per put, so a mismatch can say what differed.
    struct Record
    {
        std::vector<uint8_t> bytes;
        std::vector<std::pair<const char*, size_t>> labels; // label, offset where it starts

        auto put(const char* label, const void* data, size_t size) -> void
        {
            labels.push_back({label, bytes.size()});
            auto* p = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
        }
        template <typename T>
        auto put(const char* label, const T& value) -> void
        {
            put(label, &value, sizeof(T));
        }
        // Floating point with every NaN as one value: which NaN an operation on two NaNs returns (sign, payload)
        // follows the operand order, which the compiler may swap in a commutative operation. Whether a value is a NaN,
        // and every other bit, still has to match.
        auto put(const char* label, double value) -> void
        {
            if (std::isnan(value)) value = std::numeric_limits<double>::quiet_NaN();
            put(label, &value, sizeof(value));
        }
        auto put(const char* label, float value) -> void
        {
            if (std::isnan(value)) value = std::numeric_limits<float>::quiet_NaN();
            put(label, &value, sizeof(value));
        }
        template <size_t N>
        auto put(const char* label, const double (&values)[N]) -> void
        {
            for (size_t i = 0; i < N; ++i) put(label, values[i]);
        }
        auto clear() -> void
        {
            bytes.clear();
            labels.clear();
        }
    };

    // Result codes of the API operations, the reasons Lua would get.
    enum Code : int
    {
        Ok = 0,
        UnknownState,
        AlreadyYours,
        Taken,
        MustKeep,
        NotOwner,
        NotFinite,
        NoSlot,
    };

    class Variant
    {
      public:
        virtual ~Variant() = default;
        // The mod's start: default settings, then these published, and the switch stored as the file has it.
        virtual auto init(const HSettings& settings, bool enabled) -> void = 0;
        virtual auto publish(const HSettings& settings) -> void = 0;        // a Smoothwalker publish
        virtual auto set_enabled(bool on) -> void = 0;                      // the toggle key
        virtual auto set_modes(bool aiming, bool combat, bool traversal, Record& out) -> void = 0; // apply_position's flags
        virtual auto mode_write() -> void = 0;                              // apply_position's write
        virtual auto request_cut(int reason) -> void = 0;                   // the core's game-thread cuts
        virtual auto set_player(void* camera, void* root, int32_t translation_offset, int32_t half_height_offset) -> void = 0;
        virtual auto hook(void* self, float delta_time, void* desired_view) -> void = 0;
        virtual auto claim(int consumer, bool keep_layers, double ttl) -> int = 0;
        virtual auto release(int consumer, bool glide) -> int = 0;
        virtual auto uninstall(int consumer) -> void = 0; // on_lua_stop
        virtual auto install(int consumer) -> void = 0;   // on_lua_start, also a re-key without a stop
        virtual auto layer_set(int consumer, const LayerArgs& args) -> int = 0;
        virtual auto layer_clear(int consumer) -> int = 0;
        virtual auto take_stats(Record& out) -> void = 0;  // the log_stats report's numbers
        virtual auto panel(Record& out) -> void = 0;       // what the debug overlay and the keys read
        virtual auto state(Record& out) -> void = 0;       // everything the hook publishes
    };

    auto make_base() -> Variant*;
    auto make_new() -> Variant*;

    // The game's own GetCameraView, which the hook calls first: writes the harness's current game view.
    auto original_view(void* self, float delta_time, void* desired_view) -> void;
} // namespace harness
