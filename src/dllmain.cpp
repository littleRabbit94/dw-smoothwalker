// DWSmoothwalker: lags the character pivot the game's camera view is built around (GetCameraView, vtable
// slot 214). Design and measurements: docs/design.md.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "config.hpp"
#include "mode_tuning.hpp"
#include "smoothing.hpp"

#include <atomic>
#include <cmath>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>

#include "lua_api.hpp"

using namespace RC;
using namespace RC::Unreal;

namespace
{
    constexpr size_t GET_CAMERA_VIEW_SLOT = 214;
    // Not under scripts/: UE4SS makes a Lua mod of any folder with a scripts subfolder and logs a missing main.lua.
    constexpr const char* SETTINGS_PATH = "ue4ss/Mods/DWSmoothwalker/config/smoothwalker.ini";
    constexpr const char* PENDING_PATH = "ue4ss/Mods/DWSmoothwalker/config/smoothwalker.pending";
    constexpr const char* PRESETS_DIR = "ue4ss/Mods/DWSmoothwalker/config/presets";
    constexpr const wchar_t* PRESETS_DIR_W = L"ue4ss\\Mods\\DWSmoothwalker\\config\\presets";
    constexpr const char* MANIFEST_PATH = "ue4ss/Mods/DWSmoothwalker/mod_settings.ini";

    // Prefix of UE 5.5 FMinimalViewInfo: Location, Rotation, FOV.
    struct ViewHead
    {
        double location[3];
        double rotation[3];
        float fov;
    };
    constexpr size_t VIEW_BYTES = offsetof(ViewHead, fov) + sizeof(float); // stops at FOV: not the padding, not DesiredFOV

    // The API snapshot (lua_api.hpp): the game's view, what was handed back, and the pivot. Hook thread, numbers only.
    auto publish_api_view(const ViewHead& game, const ViewHead& shown, const dwsc::Vec3* pivot) -> void
    {
        dwapi::View g{{game.location[0], game.location[1], game.location[2]}, {game.rotation[0], game.rotation[1], game.rotation[2]}, game.fov};
        dwapi::View s{{shown.location[0], shown.location[1], shown.location[2]}, {shown.rotation[0], shown.rotation[1], shown.rotation[2]}, shown.fov};
        double p[3]{};
        if (pivot) { p[0] = pivot->x; p[1] = pivot->y; p[2] = pivot->z; }
        dwapi::publish(g, s, pivot ? p : nullptr);
    }

    // Numbers only, so the hook's copy allocates nothing on a worker thread.
    struct Tuning
    {
        double follow_rate_h, follow_rate_v;
        int curve_h, curve_v;
        double catchup_distance, min_rate_scale, max_lag_h, max_lag_v;
        bool soft_leash, rotation_smoothing, wall_clamp;
        double rotation_rate, reset_distance, reset_gap;
        double transition;   // position_transition: the crossfade after a change
        double aiming_keep;  // aiming_follow as a share: the trail and turning smoothing kept while aiming
        double combat_follow_keep;   // combat_follow as a share: the trail kept while a combat camera is up
        double combat_rotation_keep; // combat_rotation as a share: the turning smoothing kept while a combat camera is up
        double traversal_follow_keep;   // traversal_follow as a share: the trail kept while a traversal camera is up
        double traversal_rotation_keep; // traversal_rotation as a share: the turning smoothing kept while a traversal camera is up
        uint64_t generation; // bumped by a publish that changed a value
    };

    auto tuning_of(const dwsc::Settings& s) -> Tuning
    {
        return {s.follow_rate_h, s.follow_rate_v, s.curve_h, s.curve_v, s.catchup_distance, s.min_rate_scale, s.max_lag_h, s.max_lag_v,
                s.soft_leash, s.rotation_smoothing, s.wall_clamp, s.rotation_rate, s.reset_distance, s.reset_gap, s.position_transition,
                s.aiming_follow / 100.0, s.combat_follow / 100.0, s.combat_rotation / 100.0, s.traversal_follow / 100.0,
                s.traversal_rotation / 100.0, 0};
    }

    // Every field but generation.
    auto same_values(const Tuning& a, const Tuning& b) -> bool
    {
        return a.follow_rate_h == b.follow_rate_h && a.follow_rate_v == b.follow_rate_v && a.curve_h == b.curve_h && a.curve_v == b.curve_v &&
               a.catchup_distance == b.catchup_distance && a.min_rate_scale == b.min_rate_scale && a.max_lag_h == b.max_lag_h &&
               a.max_lag_v == b.max_lag_v && a.soft_leash == b.soft_leash && a.rotation_smoothing == b.rotation_smoothing &&
               a.wall_clamp == b.wall_clamp && a.rotation_rate == b.rotation_rate && a.reset_distance == b.reset_distance &&
               a.reset_gap == b.reset_gap && a.transition == b.transition && a.aiming_keep == b.aiming_keep &&
               a.combat_follow_keep == b.combat_follow_keep && a.combat_rotation_keep == b.combat_rotation_keep &&
               a.traversal_follow_keep == b.traversal_follow_keep && a.traversal_rotation_keep == b.traversal_rotation_keep;
    }

    using GetCameraViewFn = void(__fastcall*)(void* self, float delta_time, void* desired_view);

    GetCameraViewFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;
    std::atomic<int> g_in_hook{0}; // calls running inside get_camera_view_hook; unload waits for 0

    SRWLOCK g_tuning_lock = SRWLOCK_INIT;
    Tuning g_tuning = tuning_of(dwsc::Settings{});

    // Published by the game thread, read by the hook. Pointers are only compared or read under SEH.
    std::atomic<bool> g_enabled{true};
    std::atomic<bool> g_reset{true}; // a hard cut: snap, no crossfade
    std::atomic<uint64_t> g_toggle_generation{0};   // toggle key
    std::atomic<uint64_t> g_position_generation{0}; // mode writes; their FOV lands on the next camera update
    std::atomic<bool> g_log_stats{false};
    std::atomic<bool> g_log_trace{false};
    std::atomic<bool> g_aiming{false}; // an aiming camera mode is blending in or active
    std::atomic<bool> g_combat{false}; // a combat camera mode is blending in or active
    std::atomic<bool> g_traversal{false}; // a traversal camera mode is blending in or active
    std::atomic<void*> g_player_camera{nullptr};
    std::atomic<void*> g_player_root{nullptr};
    std::atomic<int32_t> g_translation_offset{-1}; // USceneComponent::ComponentToWorld.Translation
    std::atomic<int32_t> g_half_height_offset{-1};  // UCapsuleComponent::CapsuleHalfHeight; -1: the vertical follow tracks the centre

    std::atomic<uint64_t> g_frames{0};
    std::atomic<uint64_t> g_clamped{0};
    std::atomic<double> g_lag_sum{0.0};
    std::atomic<uint64_t> g_view_updates{0}; // player-camera updates: flip stages advance on these
    std::atomic<int64_t> g_last_view_qpc{0};  // preset and shoulder keys act only while this is recent
    std::atomic<double> g_view_seconds{0.0};  // world time over those updates: the flip's glide runs on it
    std::atomic<uint64_t> g_calls_timed{0};
    std::atomic<uint64_t> g_ticks_spent{0};

    // Kept free of C++ objects: __try needs a plain frame.
    auto guarded_read(void* from, void* to, size_t bytes) -> bool
    {
        __try
        {
            memcpy(to, from, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto guarded_write(void* to, const void* from, size_t bytes) -> bool
    {
        __try
        {
            memcpy(to, from, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Hook state without a lock: only the player's camera reaches it, and its calls arrive in sequence.
    struct Follow
    {
        bool valid = false;
        dwsc::Vec3 pivot_smoothed{}; // x, y: the capsule centre; z: the capsule bottom (feet), see update_view
        // A crouch or stand: the game eases its camera height over about a third of a second, and the game's own
        // vertical lag (off while the mod is on) used to delay that further. The change is lagged here through the
        // vertical follow: crouch_drop is the game's height change so far, feet-relative (root motion cancels),
        // crouch_smoothed trails it, and the difference holds the camera. An episode starts on a half-height
        // change; crouch_drop stops updating 0.6 s in (the game has settled) so a later pitch change cannot leak.
        double half_last = NAN;
        double crouch_base = NAN; // camera Z above the feet at the change; NAN: no episode
        double crouch_drop = 0.0;
        double crouch_smoothed = 0.0;
        double crouch_elapsed = 0.0;
        dwsc::Vec3 pivot_last{};
        dwsc::Quat rotation_smoothed{};
        double nominal_distance = 0.0;
        double aim = 0.0;          // 0 to 1, eased toward g_aiming: how far the follow is handed to the player's aim
        double combat = 0.0;       // 0 to 1, eased toward g_combat: how far the follow is handed to the combat camera
        double traversal = 0.0;    // 0 to 1, eased toward g_traversal: how far the follow is handed to the traversal camera
        double nominal_hold = 0.0; // s left in which nominal_distance tracks the game: a position write is gliding
        LARGE_INTEGER last_call{};

        // Last view handed to the game, relative to the game's own view that frame. The arm is rebuilt from the
        // game's camera each frame, so a fade follows moving and turning even with rotation smoothing on.
        bool out_valid = false;
        dwsc::Vec3 out_offset{};   // shown lag: pivot + arm under the output rotation - output location
        dwsc::Quat out_rotation{}; // output rotation * inverse(game rotation)
        float out_fov = NAN;

        bool blending = false;
        double blend_elapsed = 0.0, blend_duration = 0.0;
        dwsc::Vec3 from_offset{};
        dwsc::Quat from_rotation{};
        float from_fov = NAN;
        uint64_t seen_tuning = 0, seen_toggle = 0, seen_position = 0, seen_release = 0;
        bool was_owned = false; // another mod owned the camera on the last update: the falling edge is a cut
    };
    Follow g_follow;
    LARGE_INTEGER g_qpc_frequency{};

    auto seconds_between(LARGE_INTEGER a, LARGE_INTEGER b) -> double
    {
        return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g_qpc_frequency.QuadPart);
    }

    // Soft: the internal lag may run to 3x the limit and the shown lag eases into it, so the limit has no edge.
    auto leash(double lag, double limit, bool soft) -> double
    {
        if (limit <= 0.0) return 0.0;
        return soft ? limit * std::tanh(lag / limit) : std::min(lag, limit);
    }

    // How much of the wall clamp applies: 0 at 0.85 of the usual distance, 1 at 0.65 and closer. Eased because the
    // game's own modes (aiming, close combat) cross 0.85 too, and a hard threshold dropped the whole lag in one frame.
    auto wall_weight(double game_distance, double nominal_distance) -> double
    {
        if (!(nominal_distance > 0.0)) return 0.0;
        double x = std::clamp((0.85 - game_distance / nominal_distance) / 0.20, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    // Pulls result toward the game's distance from the pivot by weight; true if it moved.
    auto clamp_to_wall(dwsc::Vec3& result, const dwsc::Vec3& pivot, double game_distance, double weight) -> bool
    {
        if (weight <= 0.0) return false;
        dwsc::Vec3 out = result - pivot;
        double out_distance = dwsc::length(out);
        if (!(out_distance > game_distance) || out_distance <= 0.0) return false;
        double limit = out_distance + (game_distance - out_distance) * weight;
        result = pivot + out * (limit / out_distance);
        return true;
    }

    auto finite(const dwsc::Vec3& v) -> bool
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    auto lose_view() -> void
    {
        g_follow.valid = false;
        g_follow.out_valid = false;
        g_follow.blending = false;
        dwapi::g_blending.store(false, std::memory_order_relaxed);
    }

    // Runs with enabled false too, while the crossfade back to the game's own view is under way.
    // A per-frame trace of the vertical follow, kept in a ring and written to the log 90 frames after the capsule
    // half height changes (a crouch or a stand), so the transition sits in the middle of it. log_trace = 1. Hook
    // state, game thread only.
    struct TraceFrame
    {
        double dt, root_z, half_height, feet_z, smoothed_z, camera_z, result_z, shown_v, hold;
        bool cut;
    };
    struct Trace
    {
        static constexpr int SIZE = 240;
        TraceFrame frames[SIZE]{};
        int next = 0, count = 0;
        double last_half_height = NAN;
        int countdown = -1; // frames until the dump; -1 idle
        uint64_t dumps = 0;

        auto push(const TraceFrame& f) -> void
        {
            frames[next] = f;
            next = (next + 1) % SIZE;
            if (count < SIZE) ++count;
            if (countdown < 0 && std::isfinite(last_half_height) && std::isfinite(f.half_height) && f.half_height != last_half_height)
            {
                countdown = 90;
            }
            last_half_height = f.half_height;
            if (countdown < 0) return;
            if (--countdown >= 0) return;
            dump();
        }

        auto dump() -> void
        {
            ++dumps;
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] trace {}: {} frames; dt_ms root_z half feet_z smoothed_z camera_z result_z shown_v hold\n"),
                                           dumps, count);
            for (int i = 0; i < count; ++i)
            {
                const auto& f = frames[(next - count + i + SIZE) % SIZE];
                Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] trace {} {:3}: {:6.2f} {:9.2f} {:5.1f} {:9.2f} {:9.2f} {:9.2f} {:9.2f} {:6.2f} {:6.2f}{}\n"),
                                               dumps, i, f.dt * 1000.0, f.root_z, f.half_height, f.feet_z, f.smoothed_z, f.camera_z,
                                               f.result_z, f.shown_v, f.hold, f.cut ? STR(" cut") : STR(""));
            }
        }
    };
    Trace g_trace;

    auto update_view(void* desired_view, float delta_time, bool enabled) -> void
    {
        // Ownership is sampled once, first, and that one sample is used for the whole update. A release runs on the
        // game thread while this runs on a worker: sampling g_owner_slot after g_release_generation and g_reset
        // could see the release already published and the camera still owned, or the other way round, and write a
        // full follow offset for one frame, which turns a glide into a cut and a cut into a double snap. A lease
        // that has run out is not a claim; the game thread drops the identity the next time it looks (lua_api.hpp).
        // The slot is read before the lease, so a fresh claim never pairs with the previous owner's stale expiry.
        const bool owner_held = dwapi::g_owner_slot.load(std::memory_order_acquire) >= 0;
        const int64_t owner_expires = dwapi::g_owner_expires.load(std::memory_order_relaxed);
        const bool owned = owner_held && (owner_expires == 0 || dwapi::qpc_now() < owner_expires);
        const bool owner_keeps_layers = owned && dwapi::g_owner_keep_layers.load(std::memory_order_relaxed);
        // The release generation, sampled here because the falling edge below needs it; `changed` uses this sample.
        const auto release = dwapi::g_release_generation.load(std::memory_order_relaxed);
        const bool release_changed = release != g_follow.seen_release;
        // The camera stops being owned. A release("cut") has already set g_reset, and a release("glide") has bumped
        // the generation just read, which starts the crossfade from out_*, the game's view as the owner left it. A
        // lease running out, an uninstall or an install re-key say nothing, and writing the follow offset that piled
        // up while owned would pop the camera in that one frame and pop it again when the game thread later notices
        // and sets g_reset: those edges restart the follow from the capsule here, which is what a cut does. A glide
        // is left alone, because resetting would throw away the warm follow it is meant to ease back into and the
        // crossfade would run from nothing to nothing.
        if (g_follow.was_owned && !owned && !release_changed) g_follow.valid = false;
        g_follow.was_owned = owned;

        AcquireSRWLockShared(&g_tuning_lock);
        const Tuning t = g_tuning;
        ReleaseSRWLockShared(&g_tuning_lock);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        auto* root = g_player_root.load(std::memory_order_relaxed);
        auto offset = g_translation_offset.load(std::memory_order_relaxed);
        double pivot_raw[3]{};
        ViewHead view{};
        if (!root || offset < 0 || !guarded_read(static_cast<uint8_t*>(root) + offset, pivot_raw, sizeof(pivot_raw)) ||
            !guarded_read(desired_view, &view, VIEW_BYTES))
        {
            lose_view();
            return;
        }

        const ViewHead game_view = view; // for the API snapshot
        dwsc::Vec3 pivot{pivot_raw[0], pivot_raw[1], pivot_raw[2]};
        dwsc::Vec3 camera{view.location[0], view.location[1], view.location[2]};
        if (!finite(pivot) || !finite(camera) || !std::isfinite(view.rotation[0]) || !std::isfinite(view.rotation[1]) ||
            !std::isfinite(view.rotation[2]))
        {
            lose_view();
            return;
        }
        dwsc::Quat rotation = dwsc::from_rotator(view.rotation[0], view.rotation[1], view.rotation[2]);

        // A crouch drops the capsule centre by the half-height change in one frame (54 cm here) while the game
        // eases its own camera height down over several. Lagging the centre made the arm jump by that delta
        // and the camera pop up. The bottom of the capsule does not move in a crouch, so the vertical follow
        // tracks it: the game's eased height passes through and only real vertical travel is lagged. A missing
        // or implausible half height falls back to the centre.
        float half_height = NAN;
        auto half_offset = g_half_height_offset.load(std::memory_order_relaxed);
        if (half_offset >= 0 && !guarded_read(static_cast<uint8_t*>(root) + half_offset, &half_height, sizeof(half_height))) half_height = NAN;
        if (!std::isfinite(half_height) || half_height < 0.0f || half_height > 1000.0f) half_height = NAN;
        double feet_z = std::isfinite(half_height) ? pivot.z - half_height : pivot.z;

        // Settings, the toggle and mode writes crossfade; a hard cut (player or world change, a gap, a teleport) snaps.
        auto toggle = g_toggle_generation.load(std::memory_order_relaxed);
        auto position = g_position_generation.load(std::memory_order_relaxed);
        // A release("glide") lands here too, through the `release_changed` sampled at the top of this update: the
        // fade then starts from the game's view, because out_* tracked it while the camera was owned (lua_api.hpp).
        bool changed = t.generation != g_follow.seen_tuning || toggle != g_follow.seen_toggle ||
                       position != g_follow.seen_position || release_changed;
        // A shorter distance gliding in is not a wall. No wall clamp until it has landed.
        if (position != g_follow.seen_position) g_follow.nominal_hold = t.transition + 0.3;
        g_follow.seen_tuning = t.generation;
        g_follow.seen_toggle = toggle;
        g_follow.seen_position = position;
        g_follow.seen_release = release;
        bool cut = g_reset.exchange(false, std::memory_order_relaxed) || seconds_between(g_follow.last_call, now) > t.reset_gap ||
                   !(dwsc::length(pivot - g_follow.pivot_last) <= t.reset_distance);
        g_follow.last_call = now;
        g_follow.pivot_last = pivot;

        // The world delta, so slow motion slows the follow and the crossfade with the game. A pause is not
        // held: the camera stops updating under one, and the first update after it is a cut (reset_gap).
        double dt = std::clamp(static_cast<double>(delta_time), 0.0, 0.1);

        dwsc::Vec3 result = camera;
        dwsc::Quat result_rotation = rotation;
        double trace_shown_v = 0.0, trace_hold = 0.0;
        if (!enabled)
        {
            g_follow.valid = false; // switched back on, the follow restarts from the capsule
        }
        else if (cut || !g_follow.valid)
        {
            g_follow.valid = true;
            g_follow.aim = g_aiming.load(std::memory_order_relaxed) ? 1.0 : 0.0;
            g_follow.combat = g_combat.load(std::memory_order_relaxed) ? 1.0 : 0.0;
            g_follow.traversal = g_traversal.load(std::memory_order_relaxed) ? 1.0 : 0.0;
            g_follow.pivot_smoothed = dwsc::Vec3{pivot.x, pivot.y, feet_z};
            g_follow.rotation_smoothed = rotation;
            g_follow.half_last = static_cast<double>(half_height);
            g_follow.crouch_base = NAN;
            g_follow.crouch_drop = g_follow.crouch_smoothed = 0.0;
            g_follow.nominal_distance = dwsc::length(camera - pivot);
        }
        else
        {
            dwsc::Vec3& ps = g_follow.pivot_smoothed;
            double inner_h = t.soft_leash ? 3.0 * t.max_lag_h : t.max_lag_h;
            double inner_v = t.soft_leash ? 3.0 * t.max_lag_v : t.max_lag_v;

            double lag_hx = pivot.x - ps.x, lag_hy = pivot.y - ps.y;
            double lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
            double a_h = dwsc::follow_alpha(t.follow_rate_h, t.curve_h, lag_h, t.catchup_distance, t.min_rate_scale, dt);
            ps.x += lag_hx * a_h;
            ps.y += lag_hy * a_h;
            lag_hx = pivot.x - ps.x;
            lag_hy = pivot.y - ps.y;
            lag_h = std::sqrt(lag_hx * lag_hx + lag_hy * lag_hy);
            if (lag_h > inner_h && lag_h > 0.0)
            {
                double k = inner_h / lag_h;
                ps.x = pivot.x - lag_hx * k;
                ps.y = pivot.y - lag_hy * k;
                lag_hx *= k;
                lag_hy *= k;
                lag_h = inner_h;
            }

            double lag_v = feet_z - ps.z;
            double a_v = dwsc::follow_alpha(t.follow_rate_v, t.curve_v, std::abs(lag_v), t.catchup_distance, t.min_rate_scale, dt);
            ps.z += lag_v * a_v;
            lag_v = feet_z - ps.z;
            if (std::abs(lag_v) > inner_v)
            {
                lag_v = std::copysign(inner_v, lag_v);
                ps.z = feet_z - lag_v;
            }

            // A trail behind the crosshair reads as input lag. Only what is shown is scaled, and eased: the smoothed
            // pivot and rotation run on underneath, so the trail returns without an edge.
            double aim_target = g_aiming.load(std::memory_order_relaxed) ? 1.0 : 0.0;
            g_follow.aim += (aim_target - g_follow.aim) * (1.0 - std::exp(-8.0 * dt));
            double combat_target = g_combat.load(std::memory_order_relaxed) ? 1.0 : 0.0;
            g_follow.combat += (combat_target - g_follow.combat) * (1.0 - std::exp(-8.0 * dt));
            double traversal_target = g_traversal.load(std::memory_order_relaxed) ? 1.0 : 0.0;
            g_follow.traversal += (traversal_target - g_follow.traversal) * (1.0 - std::exp(-8.0 * dt));

            // Traversal hands the shown trail and turning to traversal_follow/traversal_rotation as it comes up;
            // combat takes over from wherever traversal left it, and aiming from wherever combat left it, smoothly.
            // Aiming wins over traversal: AimingClawRide and AntiGravAiming stack on top of ClawRide and AntiGrav.
            auto lerp = [](double a, double b, double f) { return a + (b - a) * f; };
            double aiming_keep = std::clamp(t.aiming_keep, 0.0, 1.0);
            double combat_follow_keep = std::clamp(t.combat_follow_keep, 0.0, 1.0);
            double combat_rotation_keep = std::clamp(t.combat_rotation_keep, 0.0, 1.0);
            double traversal_follow_keep = std::clamp(t.traversal_follow_keep, 0.0, 1.0);
            double traversal_rotation_keep = std::clamp(t.traversal_rotation_keep, 0.0, 1.0);
            double keep_pos = lerp(lerp(lerp(1.0, traversal_follow_keep, g_follow.traversal), combat_follow_keep, g_follow.combat), aiming_keep, g_follow.aim);
            double keep_rot = lerp(lerp(lerp(1.0, traversal_rotation_keep, g_follow.traversal), combat_rotation_keep, g_follow.combat), aiming_keep, g_follow.aim);

            double shown_h = leash(lag_h, t.max_lag_h, t.soft_leash) * keep_pos;
            double scale_h = lag_h > 0.0 ? shown_h / lag_h : 0.0;
            double shown_v = std::copysign(leash(std::abs(lag_v), t.max_lag_v, t.soft_leash), lag_v) * keep_pos;

            // The crouch hold (see Follow). Folding the running hold into a new episode keeps the output continuous
            // when a stand follows a crouch before it has settled.
            double rel = camera.z - feet_z;
            if (std::isfinite(half_height) && std::isfinite(g_follow.half_last) && static_cast<double>(half_height) != g_follow.half_last)
            {
                double running = g_follow.crouch_drop - g_follow.crouch_smoothed;
                g_follow.crouch_base = rel;
                g_follow.crouch_drop = 0.0;
                g_follow.crouch_smoothed = -running;
                g_follow.crouch_elapsed = 0.0;
            }
            if (std::isfinite(half_height)) g_follow.half_last = static_cast<double>(half_height);
            double hold = 0.0;
            if (std::isfinite(g_follow.crouch_base))
            {
                g_follow.crouch_elapsed += dt;
                if (g_follow.crouch_elapsed <= 0.6) g_follow.crouch_drop = g_follow.crouch_base - rel;
                double gap = g_follow.crouch_drop - g_follow.crouch_smoothed;
                double a_c = dwsc::follow_alpha(t.follow_rate_v, t.curve_v, std::abs(gap), t.catchup_distance, t.min_rate_scale, dt);
                g_follow.crouch_smoothed += gap * a_c;
                hold = g_follow.crouch_drop - g_follow.crouch_smoothed;
                if (g_follow.crouch_elapsed > 0.6 && std::abs(hold) < 0.1) g_follow.crouch_base = NAN;
            }
            double shown_hold = std::copysign(leash(std::abs(hold), t.max_lag_v, t.soft_leash), hold) * keep_pos;
            trace_hold = shown_hold;
            dwsc::Vec3 shown_pivot{pivot.x - lag_hx * scale_h, pivot.y - lag_hy * scale_h, pivot.z - shown_v + shown_hold};
            trace_shown_v = shown_v;

            // The arm swings with the smoothed rotation so the camera still orbits the pivot.
            dwsc::Vec3 arm = camera - pivot;
            if (t.rotation_smoothing)
            {
                double a_r = 1.0 - std::exp(-std::max(t.rotation_rate, 0.0) * dt);
                g_follow.rotation_smoothed = dwsc::slerp(g_follow.rotation_smoothed, rotation, a_r);
                // A trail past 180 degrees would catch up the short way round, which is backwards: at a turning
                // follow speed of 1 a 360 spin reversed the camera halfway (Nexus bug report, 2026-09-21). The
                // trail is capped at 90 degrees, pulled in along the same arc, so the catch-up always runs the
                // way the view turned. A single-frame turn past 180 degrees stays ambiguous, as for any smoothing.
                constexpr double MAX_TRAIL = 0.5 * 3.14159265358979323846;
                double trail = dwsc::angle_between(g_follow.rotation_smoothed, rotation);
                if (trail > MAX_TRAIL) g_follow.rotation_smoothed = dwsc::slerp(rotation, g_follow.rotation_smoothed, MAX_TRAIL / trail);
                dwsc::Quat shown = keep_rot < 1.0 ? dwsc::slerp(g_follow.rotation_smoothed, rotation, 1.0 - keep_rot) : g_follow.rotation_smoothed;
                dwsc::Quat delta = dwsc::multiply(shown, dwsc::conjugate(rotation));
                arm = dwsc::rotate(delta, arm);
                dwsc::to_rotator(shown, view.rotation[0], view.rotation[1], view.rotation[2]);
                result_rotation = shown;
            }
            else
            {
                g_follow.rotation_smoothed = rotation;
            }

            result = shown_pivot + arm;

            // The game has already pulled its camera in front of walls. While it sits closer than usual, the
            // smoothed camera may not be farther out than the game's.
            double game_distance = dwsc::length(arm);
            double settle = 1.0 - std::exp(-1.0 * dt);
            g_follow.nominal_distance = std::max(game_distance, g_follow.nominal_distance + (game_distance - g_follow.nominal_distance) * settle);
            if (g_follow.nominal_hold > 0.0)
            {
                g_follow.nominal_hold -= dt;
                g_follow.nominal_distance = game_distance;
            }
            if (t.wall_clamp && clamp_to_wall(result, pivot, game_distance, wall_weight(game_distance, g_follow.nominal_distance)))
            {
                g_clamped.fetch_add(1, std::memory_order_relaxed);
            }

            if (g_log_stats.load(std::memory_order_relaxed))
            {
                g_frames.fetch_add(1, std::memory_order_relaxed);
                g_lag_sum.store(g_lag_sum.load(std::memory_order_relaxed) + dwsc::length(pivot - shown_pivot), std::memory_order_relaxed);
            }
        }

        // Another mod owns the camera (lua_api.hpp, "authority"; `owned` was sampled at the top of this update).
        // The follow above ran and its state stays warm, but nothing of it is shown: the view goes back to exactly
        // what the game built, so out_* below record a zero offset, an identity rotation and the game's FOV and a
        // later release("glide") starts from there. No crossfade runs while owned either; the release decides how
        // the camera comes back.
        if (owned)
        {
            view = game_view;
            result = camera;
            result_rotation = rotation;
        }

        // A new lag limit, rotation smoothing or FOV would otherwise jump when it lands mid-motion.
        bool fov_ok = std::isfinite(view.fov) && view.fov > 1.0f && view.fov < 179.0f;
        if (cut || owned)
        {
            g_follow.blending = false;
        }
        else if (t.transition <= 0.0)
        {
            g_follow.blending = false; // set to 0 mid-fade: the user's choice is no fade
        }
        else if (changed && g_follow.out_valid)
        {
            g_follow.blending = true;
            g_follow.blend_elapsed = 0.0;
            g_follow.blend_duration = t.transition;
            g_follow.from_offset = g_follow.out_offset;
            g_follow.from_rotation = g_follow.out_rotation;
            g_follow.from_fov = g_follow.out_fov;
        }
        bool blended = g_follow.blending;
        if (blended)
        {
            g_follow.blend_elapsed += dt;
            double s = std::min(g_follow.blend_elapsed / g_follow.blend_duration, 1.0);
            double w = s * s * (3.0 - 2.0 * s);
            dwsc::Vec3 game_arm = camera - pivot;
            dwsc::Quat target = dwsc::multiply(result_rotation, dwsc::conjugate(rotation));
            dwsc::Quat d = dwsc::slerp(g_follow.from_rotation, target, w);
            dwsc::Vec3 lag = pivot + dwsc::rotate(target, game_arm) - result;
            result = pivot - (g_follow.from_offset + (lag - g_follow.from_offset) * w) + dwsc::rotate(d, game_arm);
            result_rotation = dwsc::multiply(d, rotation);
            dwsc::to_rotator(result_rotation, view.rotation[0], view.rotation[1], view.rotation[2]);
            if (fov_ok && std::isfinite(g_follow.from_fov)) view.fov = g_follow.from_fov + static_cast<float>((view.fov - g_follow.from_fov) * w);
            if (s >= 1.0) g_follow.blending = false;

            // The faded part of the lag was never clamped: keep it in front of a wall the game pulled in for.
            double game_distance = dwsc::length(game_arm);
            // The toggle-off fade too: it ends at the game's view, so clamping to the game's distance never moves the endpoint.
            if ((!enabled || g_follow.valid) && t.wall_clamp)
            {
                clamp_to_wall(result, pivot, game_distance, wall_weight(game_distance, g_follow.nominal_distance));
            }
        }

        bool apply_result = (enabled && !owned) || blended;
        if (apply_result)
        {
            if (!finite(result) || !std::isfinite(view.rotation[0]) || !std::isfinite(view.rotation[1]) || !std::isfinite(view.rotation[2]))
            {
                lose_view(); // leave the game's view as it built it
                return;
            }
            view.location[0] = result.x;
            view.location[1] = result.y;
            view.location[2] = result.z;
        }
        // Other mods' layers (lua_api.hpp), on top of whatever this mod did, the O switch included. While another
        // mod owns the camera they are off too, unless that owner asked to keep them.
        bool layered = (!owned || owner_keeps_layers) && dwapi::apply_layers(view.location, view.rotation, view.fov, dt);
        bool wrote = apply_result || layered;
        if (wrote)
        {
            if (!guarded_write(desired_view, &view, VIEW_BYTES))
            {
                lose_view();
                return;
            }
        }
        if (g_log_trace.load(std::memory_order_relaxed))
        {
            g_trace.push({dt, pivot.z, static_cast<double>(half_height), feet_z, g_follow.pivot_smoothed.z, camera.z, result.z, trace_shown_v, trace_hold, cut});
        }
        g_follow.out_rotation = dwsc::multiply(result_rotation, dwsc::conjugate(rotation));
        g_follow.out_offset = pivot + dwsc::rotate(g_follow.out_rotation, camera - pivot) - result;
        // Owned: the game's FOV, not a layer's, so the glide back starts from the view the owner left on screen.
        g_follow.out_fov = fov_ok ? (owned ? game_view.fov : view.fov) : NAN;
        g_follow.out_valid = true;
        dwapi::g_blending.store(g_follow.blending, std::memory_order_relaxed);
        publish_api_view(game_view, wrote ? view : game_view, &pivot);
    }

    struct InHook
    {
        InHook() { g_in_hook.fetch_add(1, std::memory_order_relaxed); }
        ~InHook() { g_in_hook.fetch_sub(1, std::memory_order_relaxed); }
    };

    void __fastcall get_camera_view_hook(void* self, float delta_time, void* desired_view)
    {
        InHook in_hook;
        g_original(self, delta_time, desired_view);
        if (self != g_player_camera.load(std::memory_order_relaxed)) return;
        g_view_updates.fetch_add(1, std::memory_order_relaxed);
        // One writer: the player's camera updates in sequence.
        if (std::isfinite(delta_time) && delta_time > 0.0f)
        {
            g_view_seconds.store(g_view_seconds.load(std::memory_order_relaxed) + delta_time, std::memory_order_relaxed);
        }
        LARGE_INTEGER stamp{};
        QueryPerformanceCounter(&stamp);
        g_last_view_qpc.store(stamp.QuadPart, std::memory_order_relaxed);
        bool enabled = g_enabled.load(std::memory_order_relaxed);
        // Off and settled: the game's view untouched. The next toggle starts from a fresh output.
        if (!enabled && !g_follow.blending && g_toggle_generation.load(std::memory_order_relaxed) == g_follow.seen_toggle &&
            !dwapi::g_layers_any.load(std::memory_order_relaxed))
        {
            g_follow.valid = false;
            g_follow.out_valid = false;
            dwapi::g_blending.store(false, std::memory_order_relaxed);
            ViewHead view{};
            if (guarded_read(desired_view, &view, VIEW_BYTES)) publish_api_view(view, view, nullptr);
            return;
        }
        if (!g_log_stats.load(std::memory_order_relaxed))
        {
            update_view(desired_view, delta_time, enabled);
            return;
        }
        LARGE_INTEGER start{}, stop{};
        QueryPerformanceCounter(&start);
        update_view(desired_view, delta_time, enabled);
        QueryPerformanceCounter(&stop);
        g_calls_timed.fetch_add(1, std::memory_order_relaxed);
        g_ticks_spent.fetch_add(static_cast<uint64_t>(stop.QuadPart - start.QuadPart), std::memory_order_relaxed);
    }

    // False under a pause, a load, a cutscene or the free camera: the player's camera is not updating.
    auto camera_live() -> bool
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        auto last = g_last_view_qpc.load(std::memory_order_relaxed);
        return last != 0 && static_cast<double>(now.QuadPart - last) / static_cast<double>(g_qpc_frequency.QuadPart) < 0.25;
    }

    auto parse_key(const std::string& name) -> int
    {
        if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        {
            return std::toupper(static_cast<unsigned char>(name[0]));
        }
        if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
        {
            int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) return 0x70 + n - 1;
        }
        return -1;
    }

    auto object_ptr(UObject* owner, const TCHAR* property) -> UObject*
    {
        if (!owner) return nullptr;
        auto** value = owner->GetValuePtrByPropertyNameInChain<UObject*>(property);
        return value ? *value : nullptr;
    }

    auto widen(const std::string& s) -> std::wstring
    {
        return std::wstring(s.begin(), s.end());
    }

    auto last_write(const char* path) -> uint64_t
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
        return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }
} // namespace

class DWSmoothwalker : public CppUserModBase
{
  public:
    DWSmoothwalker() : CppUserModBase()
    {
        ModName = STR("DWSmoothwalker");
        ModVersion = STR("0.9.0");
        dwapi::g_enabled = &g_enabled;
        dwapi::g_reset = &g_reset; // release("cut") snaps through the same flag a teleport sets
        ModDescription = STR("Frame-interpolated third-person camera");
        ModAuthors = STR("littleRabbit6");

        QueryPerformanceFrequency(&g_qpc_frequency);
        std::lock_guard guard(m_file_mutex);
        load_presets_locked();
        reload_settings_locked(true);
        // Once, without the camera_live() gate: no Mod Menu page can be open this early (docs/design.md, "Startup
        // flush"). Fixes a preset id the regenerated manifest may not list yet, before the page can fail on it.
        if (m_flush_pending.load()) flush_locked(std::chrono::steady_clock::now());
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] v{} loaded, {}\n"), ModVersion, g_enabled.load() ? STR("on") : STR("off"));
    }

    // UE4SS frees the DLL right after this (hot reload). UnregisterCallback waits for running callbacks, so the
    // game thread is out of m_tuner before restore(); a call already in the hook must return before unload.
    ~DWSmoothwalker() override
    {
        dwapi::uninstall_all();
        for (auto id : m_callbacks) Hook::UnregisterCallback(id);
        if (g_vtable_entry && g_original)
        {
            DWORD prev{};
            if (VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), PAGE_READWRITE, &prev))
            {
                // Only this mod's own entry is put back: another mod hooked after us would otherwise be unhooked too.
                if (*g_vtable_entry == reinterpret_cast<uintptr_t*>(&get_camera_view_hook))
                {
                    *g_vtable_entry = reinterpret_cast<uintptr_t*>(g_original);
                }
                else
                {
                    Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unload: slot {} no longer holds this mod's hook, left as is\n"), GET_CAMERA_VIEW_SLOT);
                }
                VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), prev, &prev);
            }
            // A worker may have read the old entry just before the restore and not entered the hook yet.
            Sleep(50);
            for (int i = 0; i < 500 && g_in_hook.load() != 0; ++i) Sleep(10);
            if (g_in_hook.load() != 0) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unload: a camera update is still in the hook\n"));
        }
        m_tuner.restore();
    }

    // The Smoothwalker table into every Lua mod's state as it starts (lua_api.hpp). Fires for each Lua mod because
    // C++ mods are started first (docs/design.md, "Checks run 2026-09-22").
    auto on_lua_start(StringViewType mod_name, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override
    {
        dwapi::install(lua.get_lua_state(), to_string(mod_name), "0.9.0");
    }

    auto on_lua_stop(StringViewType, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override
    {
        dwapi::uninstall(lua.get_lua_state());
    }

    auto on_unreal_init() -> void override
    {
        m_player_controller_name = FName(STR("BP_PlayerController_C"), FNAME_Add);

        if (!install_hook()) return;

        // UE4SS only installs BeginPlay, EndPlay and LoadMap when [Hooks] enables them (off in a "Performance"
        // profile); StaticConstructObject is always installed. Registering on an uninstalled hook logs an error,
        // so each optional hook is guarded by its flag; the engine tick covers world change, liveness and lookup.
        Hook::FCallbackOptions options{false, true, STR("DWSmoothwalker"), STR("")};
        auto& hooks = UnrealInitializer::StaticStorage::GlobalConfig;
        auto add = [&](Hook::GlobalCallbackId id) {
            if (id != Hook::ERROR_ID) m_callbacks.push_back(id);
            return id != Hook::ERROR_ID;
        };
        bool begin_play = hooks.bHookBeginPlay &&
                          add(Hook::RegisterBeginPlayPostCallback([this](auto&, AActor* actor) { on_begin_play(actor); }, options));
        bool end_play = hooks.bHookEndPlay &&
                        add(Hook::RegisterEndPlayPostCallback([this](auto&, AActor* actor, EEndPlayReason) { on_end_play(actor); }, options));
        bool load_map = hooks.bHookLoadMap && add(Hook::RegisterLoadMapPreCallback(
                                                      [this](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) { forget_world(); },
                                                      options));
        // Runs on whatever thread constructs the object (async loading threads too): only a flag test, a pointer
        // and an FName compare and, on a match, an index read and a locked hand-off (a mode pushed on the
        // player's camera into m_tuner, the controller into m_new_controller).
        bool new_object = add(Hook::RegisterStaticConstructObjectPostCallback(
                [this](auto& info, const FStaticConstructObjectParameters& params) {
                    if (static_cast<uint32_t>(params.SetFlags) & static_cast<uint32_t>(RF_ClassDefaultObject | RF_ArchetypeObject)) return;
                    auto* cls = const_cast<UClass*>(params.Class);
                    if (!cls) return;
                    auto* camera = g_player_camera.load(std::memory_order_relaxed);
                    if (camera && params.Outer == camera)
                    {
                        if (auto* mode = info.GetCurrentResolvedReturnValue()) m_tuner.note_new(dwsc::LiveRef::of(mode));
                        return;
                    }
                    if (cls->GetNamePrivate() != m_player_controller_name) return;
                    auto* object = info.GetCurrentResolvedReturnValue();
                    if (!object) return;
                    std::lock_guard guard(m_new_controller_mutex);
                    m_new_controller = dwsc::LiveRef::of(object);
                    m_new_controller_pending.store(true);
                }, options));
        bool engine_tick =
                hooks.bHookEngineTick && add(Hook::RegisterEngineTickPostCallback([this](auto&, UEngine* engine, float, bool) { on_engine_tick(engine); }, options));
        auto state = [](bool on) { return on ? STR("on") : STR("off"); };
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] player discovery: new-object callback {}, BeginPlay {}, EndPlay {}, LoadMap {}; engine tick "
                                           "checks world and liveness, FindFirstOf fallback from 2 s backing off to 60 s without a controller\n"),
                                       state(new_object), state(begin_play), state(end_play), state(load_map));
        if (!engine_tick) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] UE4SS EngineTick hook is off: the camera cannot find the player\n"));

        bind(m_toggle_key, STR("toggle_key"), [this]() {
            std::lock_guard guard(m_file_mutex);
            bool now = !g_enabled.load();
            set_enabled_locked(now);
            publish_locked();
            mark_pending_locked();
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] smoothing {}\n"), now ? STR("on") : STR("off"));
            request_banner(now ? STR("Smoothwalker: On") : STR("Smoothwalker: Off"));
        });
        bind(m_preset_key, STR("preset_key"), [this]() {
            if (key_live(STR("preset"))) cycle_preset();
        });
        bind(m_shoulder_key, STR("shoulder_key"), [this]() {
            if (key_live(STR("shoulder"))) swap_shoulder();
        });

        m_find_requested.store(true); // after a hot reload the controller has already begun play
        m_last_report = m_last_poll = std::chrono::steady_clock::now();
    }

    auto on_update() -> void override
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - m_last_poll).count() >= 0.25)
        {
            m_last_poll = now;
            std::lock_guard guard(m_file_mutex);
            if (last_write(SETTINGS_PATH) != m_settings_stamp) reload_settings_locked(false);
        }
        // After the poll, so a file change is applied before the live values are written over it. Written while the
        // camera is live, or paused with the Mod Menu closed: a page reopened from the pause menu then shows the
        // loaded values, and no page is open to refuse its next Apply over the write.
        if (m_flush_pending.load() && now >= m_next_flush && (camera_live() || !mod_menu_open()))
        {
            std::lock_guard guard(m_file_mutex);
            flush_locked(now);
        }

        if (!g_log_stats.load()) return;
        auto elapsed = std::chrono::duration<double>(now - m_last_report).count();
        if (elapsed < 5.0) return;
        m_last_report = now;
        auto frames = g_frames.exchange(0);
        auto clamped = g_clamped.exchange(0);
        auto lag = g_lag_sum.exchange(0.0);
        auto timed = g_calls_timed.exchange(0);
        auto ticks = g_ticks_spent.exchange(0);
        double micros = timed ? 1e6 * static_cast<double>(ticks) / static_cast<double>(g_qpc_frequency.QuadPart) / timed : 0.0;
        Output::send<LogLevel::Normal>(
                STR("[DWSmoothwalker] {:.1f} smoothed frames/s, mean shown lag {:.1f} cm, wall clamp {:.0f}%, {:.1f} us per frame in the hook\n"),
                frames / elapsed, frames ? lag / frames : 0.0, frames ? 100.0 * clamped / frames : 0.0, micros);
    }

  private:
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    FName m_player_controller_name{};
    // Game thread only, each checked against the object array every engine tick before use.
    dwsc::LiveRef m_controller, m_pawn, m_camera, m_root;
    std::atomic<bool> m_player_known{false}; // m_controller held; read by request_banner on the UE4SS update thread
    int32_t m_pawn_offset = -1; // AController::Pawn, same class every map
    bool m_offset_retry = false; // find_translation_offset failed for m_pawn; game thread only
    std::chrono::steady_clock::time_point m_next_offset_scan{};
    std::chrono::seconds m_offset_wait{2};
    std::atomic<bool> m_find_requested{false};
    std::chrono::steady_clock::time_point m_next_find{};
    std::chrono::seconds m_find_interval{2}; // FindFirstOf fallback: 2 s, doubling to 60 s while nothing is found
    std::mutex m_new_controller_mutex;          // m_new_controller: written by the new-object callback on any thread
    dwsc::LiveRef m_new_controller;
    std::atomic<bool> m_new_controller_pending{false};
    int32_t m_viewport_offset = -1, m_world_offset = -1; // UEngine::GameViewport, UGameViewportClient::World; -2 absent
    UObject* m_world = nullptr;                 // compared only, never followed
    bool m_world_seen = false;

    std::mutex m_file_mutex; // the config files, m_settings, m_baseline, m_presets, m_loaded_id; the engine tick never takes it
    dwsc::Settings m_settings{};
    uint64_t m_settings_stamp = 0;                // write time of smoothwalker.ini as last read or written
    std::map<std::string, double> m_baseline;     // each numeric key as last known to be in smoothwalker.ini
    int m_loaded_id = 0;                          // last preset loaded or cycled; shown while it still matches
    UClass* m_activatable_class = nullptr;        // CommonActivatableWidget, the Mod Menu's host class
    UObject* m_menu_host = nullptr;               // the Mod Menu host last seen open; checked before any rescan
    bool m_menu_logged = false;
    bool m_custom_pinned = false;                 // Custom was picked: shown until a preset is loaded or a slot adopted
    std::vector<dwsc::Preset> m_presets;          // built-ins, slots, drop-ins, in cycle order; scanned once per session
    std::string m_pending_file;                   // content last written to smoothwalker.pending; empty when none
    std::atomic<bool> m_flush_pending{false};     // live settings differ from m_baseline
    bool m_flush_failing = false;                 // a write-back failed; warned once until one succeeds
    std::chrono::steady_clock::time_point m_next_flush{};
    std::string m_toggle_key, m_preset_key, m_shoulder_key;

    dwsc::ModeTuner m_tuner; // game thread only
    std::mutex m_position_mutex;
    dwsc::PositionTuning m_position{};
    std::atomic<uint64_t> m_position_generation{1};
    uint64_t m_position_applied_generation = 0; // game thread only

    std::chrono::steady_clock::time_point m_last_report{}, m_last_poll{};
    uint64_t m_tuning_generation = 0; // under g_tuning_lock

    std::mutex m_banner_mutex; // m_banner_text, m_banner_due, m_banner_pending
    std::wstring m_banner_text;
    std::chrono::steady_clock::time_point m_banner_due{};
    bool m_banner_pending = false;
    int32_t m_queue_offset = -1;        // NotificationSubsystem::NotificationQueue
    int32_t m_region_data_offset = -1;  // RegionEnteredNotificationInfo::RegionData
    UClass* m_region_info_class = nullptr;
    std::atomic<bool> m_show_banner{true};
    int m_banner_state = 0; // 0 unresolved, 1 ready, -1 unavailable (game thread only)
    UFunction* m_banner_function = nullptr;
    UObject* m_banner_library = nullptr;
    dwsc::LiveRef m_notifications; // NotificationSubsystem, game thread only, checked live before use

    // Debounced: a burst of presses shows one banner, with the last text.
    // Dropped without a player: a banner queued at the main menu would show minutes later, after a load.
    auto request_banner(std::wstring text) -> void
    {
        if (!m_show_banner.load() || !m_player_known.load()) return;
        std::lock_guard guard(m_banner_mutex);
        m_banner_text = std::move(text);
        m_banner_due = std::chrono::steady_clock::now() + BANNER_SETTLE;
        m_banner_pending = true;
    }

    static constexpr auto BANNER_SETTLE = std::chrono::milliseconds(400);

    struct ObjectArray
    {
        UObject** data;
        int32_t num;
        int32_t max;
    };

    // Thins our own waiting banners so presses cannot build a backlog. The banner on screen is left alone:
    // ending a notification its widget is showing crashed the game (docs/design.md, "Rapid banners queue").
    // Ours are recognised by class and text, never by a remembered address, which the game may reuse.
    auto drop_stale_banners(UObject* subsystem) -> void
    {
        if (!subsystem) return;
        if (m_queue_offset < 0 || m_region_data_offset < 0 || !m_region_info_class)
        {
            auto* slot = subsystem->GetValuePtrByPropertyNameInChain<void>(STR("NotificationQueue"));
            m_region_info_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/DogwoodUI.RegionEnteredNotificationInfo"));
            int32_t data = -1;
            if (m_region_info_class)
            {
                for (FProperty* property : m_region_info_class->ForEachProperty())
                {
                    if (property->GetName() == STR("RegionData")) data = property->GetOffset_ForInternal();
                }
            }
            if (!slot || data < 0) return;
            m_queue_offset = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(subsystem));
            m_region_data_offset = data;
        }
        auto* queue = reinterpret_cast<ObjectArray*>(reinterpret_cast<uint8_t*>(subsystem) + m_queue_offset);
        if (queue->num < 0 || queue->num > queue->max || queue->max > 4096 || (queue->num > 0 && !queue->data)) return;

        int32_t kept = 0;
        for (int32_t i = 0; i < queue->num; ++i)
        {
            auto* entry = queue->data[i];
            if (!is_our_banner(entry)) queue->data[kept++] = entry;
        }
        queue->num = kept;
    }

    auto is_our_banner(UObject* entry) -> bool
    {
        if (!entry || entry->GetClassPrivate() != m_region_info_class) return false;
        auto* text = reinterpret_cast<FText*>(reinterpret_cast<uint8_t*>(entry) + m_region_data_offset + BANNER_TEXT);
        return text->ToString().starts_with(BANNER_PREFIX);
    }

    static constexpr const wchar_t* BANNER_PREFIX = L"Smoothwalker:";

    // The region banner shows RegionData.RegionDisplayText. The hard-coded parameter layout must match
    // reflection, or banners stay off.
    auto resolve_banner() -> bool
    {
        m_banner_state = -1;
        m_banner_function = UObjectGlobals::StaticFindObject<UFunction*>(
                nullptr, nullptr, STR("/Script/DogwoodUI.NotificationSystemLibrary:PushRegionEnteredNotification"));
        m_banner_library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/DogwoodUI.Default__NotificationSystemLibrary"));
        auto* region = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/DogwoodSystem.RegionData"));
        if (!m_banner_function || !m_banner_library || !region)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] notification function not found, banners off\n"));
            return false;
        }

        auto offset_of = [](UStruct* owner, const wchar_t* name) -> int32_t {
            for (FProperty* property : owner->ForEachProperty())
            {
                if (property->GetName() == name) return property->GetOffset_ForInternal();
            }
            return -1;
        };
        auto world = offset_of(m_banner_function, STR("WorldContextObject"));
        auto data = offset_of(m_banner_function, STR("RegionData"));
        auto flag = offset_of(m_banner_function, STR("IsNewlyDiscovered"));
        auto text = offset_of(region, STR("RegionDisplayText"));
        if (world != BANNER_WORLD || data != BANNER_DATA || flag != BANNER_FLAG || text != BANNER_TEXT)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] notification layout changed ({}, {}, {}, {}), banners off\n"), world, data, flag, text);
            return false;
        }
        m_banner_state = 1;
        return true;
    }

    auto show_pending_banner() -> void
    {
        if (!m_controller.object) return; // checked live this tick
        std::wstring line;
        {
            std::lock_guard guard(m_banner_mutex);
            if (!m_banner_pending || std::chrono::steady_clock::now() < m_banner_due) return;
            m_banner_pending = false;
            line = m_banner_text;
        }
        if (m_banner_state == 0) resolve_banner();
        if (m_banner_state != 1) return;

        // Cached: FindFirstOf walks the whole object array (28 ms measured), and this runs mid-glide after a preset change.
        if (!m_notifications.alive()) m_notifications = dwsc::LiveRef::of(UObjectGlobals::FindFirstOf(STR("NotificationSubsystem")));
        drop_stale_banners(m_notifications.object);
        FText text(line.c_str());
        uint8_t params[BANNER_PARAMS_SIZE]{};
        memcpy(params + BANNER_WORLD, &m_controller.object, sizeof(m_controller.object));
        text.CopyBorrowedTo(params + BANNER_DATA + BANNER_TEXT);
        params[BANNER_FLAG] = 0; // not newly discovered: no discovery reward
        m_banner_library->ProcessEvent(m_banner_function, params);
    }

    static constexpr int32_t BANNER_WORLD = 0x00;
    static constexpr int32_t BANNER_DATA = 0x08;
    static constexpr int32_t BANNER_TEXT = 0x20; // inside RegionData
    static constexpr int32_t BANNER_FLAG = 0x40;
    static constexpr size_t BANNER_PARAMS_SIZE = 0x48;

    auto bind(const std::string& name, const TCHAR* setting, std::function<void()> action) -> void
    {
        if (name.empty()) return;
        int key = parse_key(name);
        if (key < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] unknown {} '{}', not bound\n"), setting, widen(name));
            return;
        }
        register_keydown_event(static_cast<Input::Key>(key), std::move(action));
    }

    // The preset and shoulder keys act only while the mod is on and the camera is live. Off is the game as shipped,
    // so nothing may move; under a pause the change would land in smoothwalker.ini behind an open Mod Menu page.
    auto key_live(const TCHAR* key) -> bool
    {
        const TCHAR* why = !g_enabled.load() ? STR("Smoothwalker is off") : !camera_live() ? STR("the camera is paused") : nullptr;
        if (why) Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] {} key ignored while {}\n"), key, why);
        return !why;
    }

    // The live switch, kept in m_settings too so the write-back shows it on the Mod Menu page.
    auto set_enabled_locked(bool on) -> void
    {
        m_settings.enabled = on;
        if (g_enabled.load() == on) return;
        g_toggle_generation.fetch_add(1); // before the store: a hook frame between the two must still fade
        g_enabled.store(on);
    }

    auto publish_locked() -> void
    {
        // No g_reset: the hook crossfades on the new generation instead of snapping the lag away mid-motion.
        // Only a changed value starts one: a shoulder swap and the switches change none, and a fade holds part of the old lag.
        auto tuning = tuning_of(m_settings);
        AcquireSRWLockExclusive(&g_tuning_lock);
        if (!same_values(tuning, g_tuning))
        {
            tuning.generation = ++m_tuning_generation;
            g_tuning = tuning;
        }
        ReleaseSRWLockExclusive(&g_tuning_lock);
        g_log_stats.store(m_settings.log_stats);
        g_log_trace.store(m_settings.log_trace);
        m_show_banner.store(m_settings.show_banner);

        // enabled off is the game as shipped, camera modes and its own lag included (position_of).
        auto position = dwsc::position_of(m_settings);
        std::lock_guard guard(m_position_mutex);
        // With camera_tuning off only the lag switch is written, so a changed position number applies nothing.
        // The values are kept, so switching it on applies the latest.
        auto written = [](dwsc::PositionTuning p) {
            if (p.active) return p;
            dwsc::PositionTuning lag_only;
            lag_only.active = false;
            lag_only.own_lag = p.own_lag;
            return lag_only;
        };
        if (!(written(position) == written(m_position))) m_position_generation.fetch_add(1);
        m_position = position;
    }

    // Written back to the ini by flush_locked, so the Mod Menu page shows it.
    auto swap_shoulder() -> void
    {
        std::lock_guard guard(m_file_mutex);
        m_settings.shoulder_swap = !m_settings.shoulder_swap;
        update_active_locked();
        publish_locked();
        mark_pending_locked();
        // No banner: the camera moving to the other shoulder is the feedback.
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] shoulder {}{}\n"), m_settings.shoulder_swap ? STR("swapped") : STR("as the game has it"),
                                       m_settings.camera_tuning ? STR("") : STR(" (camera_tuning is 0: shows once it is on)"));
    }

    // Game thread.
    auto apply_position() -> void
    {
        auto* camera = static_cast<UObject*>(g_player_camera.load());
        m_tuner.tick(g_view_updates.load(), g_view_seconds.load(), camera);
        // off: no per-tick GetState calls either
        auto state = camera && g_enabled.load() ? m_tuner.mode_state() : dwsc::ModeState{};
        g_aiming.store(state.aiming);
        if (state.combat != g_combat.exchange(state.combat))
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] combat camera {}\n"), state.combat ? STR("on") : STR("off"));
        if (state.traversal != g_traversal.exchange(state.traversal))
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] traversal camera {}\n"), state.traversal ? STR("on") : STR("off"));
        if (!camera) return;
        auto generation = m_position_generation.load();
        if (generation == m_position_applied_generation) return;
        dwsc::PositionTuning position;
        {
            std::lock_guard guard(m_position_mutex);
            position = m_position;
        }
        m_tuner.apply(position, camera);
        m_position_applied_generation = generation;
        g_position_generation.fetch_add(1); // mode FOV lands on the next camera update: crossfade it
    }

    // Startup parses every value; a later change applies only keys whose number moved since the file was last
    // seen, so a live key or preset change survives an Apply of the rest. Only flush_locked writes smoothwalker.ini.
    auto reload_settings_locked(bool startup) -> void
    {
        // Stamp first: a write landing between the two is then seen by the next poll.
        auto stamp = last_write(SETTINGS_PATH);
        auto content = dwsc::read_file(SETTINGS_PATH);
        // At startup the key names are read only once, so ride out a Mod Menu rename in progress.
        for (int i = 0; startup && !content && i < 10; ++i)
        {
            Sleep(20);
            stamp = last_write(SETTINGS_PATH);
            content = dwsc::read_file(SETTINGS_PATH);
        }
        if (!content)
        {
            // The Mod Menu replaces the file by rename, so it is briefly absent: keep what is live and retry.
            if (!startup) return;
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] smoothwalker.ini not found, using defaults\n"));
            m_settings = dwsc::Settings{};
            m_toggle_key = m_settings.toggle_key;
            m_preset_key = m_settings.preset_key;
            m_shoulder_key = m_settings.shoulder_key;
            publish_locked();
            return;
        }
        m_settings_stamp = stamp;
        auto file = dwsc::parse_numbers(*content);

        if (startup)
        {
            m_settings = dwsc::parse_settings(*content);
            m_toggle_key = m_settings.toggle_key;
            m_preset_key = m_settings.preset_key;
            m_shoulder_key = m_settings.shoulder_key;
            m_baseline = std::move(file);
            apply_pending_file_locked();
            // Not a load request: only which of several matching presets to show.
            m_loaded_id = m_settings.preset;
            bool custom = m_settings.preset == 0;
            update_active_locked();
            // Custom in the file over values a preset matches: Custom was picked, so it stays (and saves into no slot).
            if (custom && m_settings.preset != 0)
            {
                m_custom_pinned = true;
                m_settings.preset = 0;
            }
            publish_locked();
            g_enabled.store(m_settings.enabled);
            mark_pending_locked();
            return;
        }

        dwsc::Values edits;
        for (auto& [key, value] : file)
        {
            auto known = m_baseline.find(key);
            if (known == m_baseline.end() || known->second != value) edits.emplace_back(key, value);
            m_baseline[key] = value;
        }
        if (edits.empty())
        {
            mark_pending_locked(); // numbers unchanged; the stamp still moved
            return;
        }
        auto edited = [&](const char* key) {
            return std::any_of(edits.begin(), edits.end(), [&](auto& edit) { return edit.first == key; });
        };
        auto is_slot = [](int id) { return id >= 1 && id <= dwsc::MAX_SLOTS; };
        int active_before = m_settings.preset; // the slot an Apply that leaves the picker alone saves into

        // (a) Ordinary edits onto the live settings. The toggle's state changes only if the file's enabled did.
        dwsc::apply_values(m_settings, edits);
        if (edited("enabled")) set_enabled_locked(m_settings.enabled); // fades like the toggle key

        dwsc::Values own; // the preset keys this Apply moved
        for (auto& edit : edits)
        {
            if (dwsc::is_preset_key(edit.first)) own.push_back(edit);
        }
        auto slot_name = [&](int id) {
            auto* slot = find_preset(id);
            return dwsc::to_wide(slot ? slot->name : "Slot " + std::to_string(id));
        };
        // The slot file is written now: the menu does not watch it.
        auto save_slot = [&](int id) {
            bool ok = save_slot_locked(id, dwsc::preset_of(m_settings));
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] saved slot {}{}\n"), id, ok ? STR("") : STR(": write failed"));
            return ok;
        };

        int picked = m_settings.preset;
        if (edited("preset") && is_slot(picked) && !find_preset(picked))
        {
            // (b) An empty slot adopts the live values, this Apply's slider edits included. Not derived: the
            // menu already wrote preset = N, so there is nothing to flush at once.
            if (save_slot(picked))
            {
                m_loaded_id = picked;
                m_custom_pinned = false;
                request_banner(STR("Smoothwalker: saved to ") + slot_name(picked));
            }
        }
        else if (edited("preset") && picked != 0)
        {
            // (c) A changed picker loads that preset. Preset keys edited in the same Apply win over it, and on a
            // slot they go straight back into it. The live numbers now differ from what the Apply wrote; on_update
            // writes them once the menu closes (the open page would refuse its next Apply over the write), so a page
            // reopened from the pause menu shows the loaded values. Until then the open page's sliders are stale;
            // an Apply there still works and moves only the keys it changed.
            if (load_preset_locked(picked))
            {
                dwsc::apply_values(m_settings, own);
                if (is_slot(picked) && !own.empty()) save_slot(picked);
            }
        }
        else if (edited("preset"))
        {
            // Custom: detached from whatever was active, nothing saved. Pinned, or the values would match the slot
            // again and the next edit would be saved into it.
            m_loaded_id = 0;
            m_custom_pinned = true;
        }
        else if (is_slot(active_before) && !own.empty())
        {
            // (d) An edit with a slot active is saved into it. Built-ins and drop-ins stay read-only: an edit
            // there just falls through to update_active_locked, which gives Custom.
            if (save_slot(active_before))
            {
                m_loaded_id = active_before;
                request_banner(STR("Smoothwalker: ") + slot_name(active_before) + STR(" updated"));
            }
        }

        update_active_locked();
        publish_locked();
        mark_pending_locked();
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] settings applied\n"));
    }

    // Once per session, in the constructor, before anything reads presets or the Mod Menu reads mod_settings.ini:
    // built-ins, slots 1..MAX_SLOTS ("Slot N.ini"), then other presets-folder *.ini as drop-ins (201+, by name).
    // On a failed manifest rewrite drop-ins stay off, so the indicator never shows an id the page lacks.
    auto load_presets_locked() -> void
    {
        m_presets.clear();
        for (auto& p : dwsc::builtin_presets()) m_presets.push_back({p.id, p.name, p.values});
        CreateDirectoryW(PRESETS_DIR_W, nullptr); // so it is there to drop files into; fails harmlessly if present

        auto files = dwsc::list_preset_files(PRESETS_DIR_W);
        auto read_preset = [&](const std::wstring& name) -> std::optional<std::pair<std::string, dwsc::Values>> {
            auto content = dwsc::read_small_file(std::wstring(PRESETS_DIR_W) + L"\\" + name, dwsc::MAX_PRESET_FILE);
            if (!content)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] presets/{}: unreadable or over 64 KiB, skipped\n"), name);
                return std::nullopt;
            }
            auto parsed = dwsc::parse_preset_file(std::move(*content));
            if (parsed.second.empty())
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] presets/{}: no preset settings, skipped\n"), name);
                return std::nullopt;
            }
            return parsed;
        };

        for (auto& [slot, name] : files.slots)
        {
            // The file's name line is the slot's display name; the file name stays "Slot N.ini".
            if (auto parsed = read_preset(name))
            {
                m_presets.push_back({slot, dwsc::display_name(parsed->first, "Slot " + std::to_string(slot), slot), dwsc::normalize_preset(parsed->second)});
            }
        }

        std::vector<dwsc::Preset> dropins;
        size_t over_limit = 0;
        for (auto& name : files.dropins)
        {
            if (dropins.size() >= static_cast<size_t>(dwsc::MAX_DROPINS))
            {
                ++over_limit;
                continue;
            }
            auto parsed = read_preset(name);
            if (!parsed) continue;
            int id = dwsc::FIRST_DROPIN_ID + static_cast<int>(dropins.size());
            auto stem = dwsc::utf8_of(name.substr(0, name.size() - 4)).value_or("");
            dropins.push_back({id, dwsc::display_name(parsed->first, stem, id), dwsc::normalize_preset(parsed->second)});
        }
        if (over_limit)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {} presets over the limit of {} skipped\n"), over_limit, dwsc::MAX_DROPINS);
        }

        // Picker order: saved slots, drop-ins, then the empty slots, so nothing empty sits between the presets that
        // load. Empty slots stay listed: the page fails to open if the ini's preset id is not among the values, and
        // a slot saved this session becomes the active id.
        std::string values = "0|101|102|103", labels = "Custom|Tight|Balanced|Cinematic";
        std::string empty_values, empty_labels;
        for (int n = 1; n <= dwsc::MAX_SLOTS; ++n)
        {
            auto id = std::to_string(n);
            auto* saved = find_preset(n); // m_presets holds the built-ins and the slots here
            if (saved)
            {
                values += "|" + id;
                labels += "|" + saved->name;
            }
            else
            {
                empty_values += "|" + id;
                empty_labels += "|Slot " + id + " (empty)";
            }
        }
        for (auto& p : dropins)
        {
            values += "|" + std::to_string(p.id);
            labels += "|" + p.name;
        }
        values += empty_values;
        labels += empty_labels;
        bool listed = false;
        if (auto manifest = dwsc::read_file(MANIFEST_PATH))
        {
            if (auto updated = dwsc::with_preset_choices(*manifest, "[Setting.preset]", values, labels))
            {
                listed = *updated == *manifest || dwsc::write_file(MANIFEST_PATH, *updated);
                if (!listed) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not write mod_settings.ini\n"));
            }
            else
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] mod_settings.ini: [Setting.preset] PresetValues or PresetLabels missing\n"));
            }
        }
        else
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] mod_settings.ini not found\n"));
        }
        if (!listed && !dropins.empty())
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] {} presets from the presets folder off for this session: the Mod Menu page could not list them\n"),
                                            dropins.size());
            dropins.clear();
        }
        for (auto& p : dropins)
        {
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] preset {} from the presets folder: {}\n"), p.id, dwsc::to_wide(p.name));
        }
        size_t slots = m_presets.size() - dwsc::builtin_presets().size();
        m_presets.insert(m_presets.end(), std::make_move_iterator(dropins.begin()), std::make_move_iterator(dropins.end()));
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] presets: {} saved slots, {} from the presets folder\n"), slots,
                                       m_presets.size() - slots - dwsc::builtin_presets().size());
    }

    // The Dawnwalker Mod Menu (Nexus 271) creates its host as a plain CommonActivatableWidget (main.lua, library:Create
    // with the native class, which the game's own screens all subclass), enabled and shown while the menu is open and
    // Collapsed once closed, when it lingers until GC. Any such widget not Collapsed means a settings page may be open.
    // Only reached with a write pending and the camera not live. A menu that changes its host is not recognised, and
    // the write then lands under the open page as it did before 0.9: the page refuses its next Apply until reopened.
    auto mod_menu_open() -> bool
    {
        constexpr uint8_t COLLAPSED = 1; // ESlateVisibility
        auto shown = [&](UObject* object) -> bool {
            auto* visibility = object->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
            return visibility && *visibility != COLLAPSED;
        };
        if (!m_activatable_class)
        {
            m_activatable_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/CommonUI.CommonActivatableWidget"));
            if (!m_activatable_class) return false;
        }
        if (m_menu_host && !m_menu_host->IsUnreachable() && m_menu_host->GetClassPrivate() == m_activatable_class && shown(m_menu_host)) return true;
        m_menu_host = nullptr; // closed or gone; every open creates a new host
        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
            if (!object || object->GetClassPrivate() != m_activatable_class || object->IsUnreachable() || !shown(object)) return LoopAction::Continue;
            m_menu_host = object;
            return LoopAction::Break;
        });
        if (m_menu_host && !m_menu_logged)
        {
            m_menu_logged = true;
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] Mod Menu open: {}\n"), m_menu_host->GetFullName());
        }
        return m_menu_host != nullptr;
    }

    auto save_slot_locked(int slot, dwsc::Values values) -> bool
    {
        values = dwsc::normalize_preset(values);
        CreateDirectoryA(PRESETS_DIR, nullptr);
        auto path = std::string(PRESETS_DIR) + "/Slot " + std::to_string(slot) + ".ini";
        auto* known = find_preset(slot);
        auto name = known ? known->name : "Slot " + std::to_string(slot); // a renamed slot keeps its name
        if (!dwsc::write_file(path, dwsc::slot_file_content(slot, name, values))) return false;
        if (known)
        {
            known->values = std::move(values);
            return true;
        }
        auto is_after = [&](const dwsc::Preset& p) { return (p.id >= 1 && p.id <= dwsc::MAX_SLOTS && p.id > slot) || p.id >= dwsc::FIRST_DROPIN_ID; };
        m_presets.insert(std::find_if(m_presets.begin(), m_presets.end(), is_after), dwsc::Preset{slot, "Slot " + std::to_string(slot), std::move(values)});
        return true;
    }

    auto find_preset(int id) -> dwsc::Preset*
    {
        auto it = std::find_if(m_presets.begin(), m_presets.end(), [&](const dwsc::Preset& p) { return p.id == id; });
        return it != m_presets.end() ? &*it : nullptr;
    }

    // Applies only the keys the preset holds; a hand-edited file may hold fewer.
    auto load_preset_locked(int id) -> bool
    {
        auto* preset = find_preset(id);
        if (!preset)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] preset {} is empty, nothing loaded\n"), id);
            return false;
        }
        dwsc::apply_values(m_settings, preset->values);
        m_loaded_id = id;
        m_custom_pinned = false;
        auto name = dwsc::to_wide(preset->name);
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] loaded preset {}\n"), name);
        request_banner(STR("Smoothwalker: ") + name);
        return true;
    }

    // m_settings.preset becomes the preset the live settings match, preferring the last one loaded, else 0 (Custom).
    auto update_active_locked() -> void
    {
        auto matches = [&](const dwsc::Preset& p) {
            return !p.values.empty() && std::all_of(p.values.begin(), p.values.end(), [&](auto& entry) {
                return std::abs(dwsc::number_of(m_settings, entry.first) - entry.second) <= 1e-4;
            });
        };
        if (m_custom_pinned)
        {
            m_settings.preset = 0;
            return;
        }
        int active = 0;
        if (auto* loaded = m_loaded_id != 0 ? find_preset(m_loaded_id) : nullptr; loaded && matches(*loaded)) active = m_loaded_id;
        for (auto& p : m_presets)
        {
            if (!active && matches(p)) active = p.id;
        }
        m_settings.preset = active;
    }

    // Built-ins, saved slots, then drop-ins, starting after the active preset (at the first from Custom).
    auto cycle_preset() -> void
    {
        std::lock_guard guard(m_file_mutex);
        auto at = std::find_if(m_presets.begin(), m_presets.end(), [&](const dwsc::Preset& p) { return p.id == m_settings.preset; });
        size_t next = m_settings.preset == 0 || at == m_presets.end() ? 0 : (static_cast<size_t>(at - m_presets.begin()) + 1) % m_presets.size();
        if (!load_preset_locked(m_presets[next].id)) return;
        update_active_locked();
        publish_locked();
        mark_pending_locked();
    }

    // The live numbers that differ from the file; preset is written as m_settings holds it (the active preset).
    // Compared as written (%.6g), so a value the file cannot hold exactly is not rewritten forever.
    auto pending_writes_locked() -> dwsc::Values
    {
        dwsc::Values writes;
        for (auto* key : dwsc::NUMERIC_KEYS)
        {
            double value = dwsc::number_of(m_settings, key);
            auto known = m_baseline.find(key);
            if (known == m_baseline.end() || dwsc::format_number(known->second) != dwsc::format_number(value)) writes.emplace_back(key, value);
        }
        return writes;
    }

    // After any change to the live settings, the baseline or the stamp. Marks the write-back and mirrors it into
    // smoothwalker.pending (which the Mod Menu does not read), so a preset loaded from a paused menu survives an exit
    // or hot reload before the camera goes live. The side file is rewritten only when its content changes.
    auto mark_pending_locked() -> void
    {
        auto writes = pending_writes_locked();
        m_flush_pending.store(!writes.empty());
        std::string content;
        if (!writes.empty())
        {
            content = "; DWSmoothwalker: settings not yet written to smoothwalker.ini. Applied at the next start only while\n"
                      "; smoothwalker.ini's write time still equals stamp.\n"
                      "stamp = " + std::to_string(m_settings_stamp) + "\n";
            char buffer[64];
            for (auto& [key, value] : writes)
            {
                std::snprintf(buffer, sizeof(buffer), "%.17g", value);
                content += key + " = " + buffer + "\n";
            }
        }
        if (content == m_pending_file) return;
        if (content.empty())
        {
            DeleteFileA(PENDING_PATH);
        }
        else if (!dwsc::write_file(PENDING_PATH, content))
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not write smoothwalker.pending\n"));
            return;
        }
        m_pending_file = std::move(content);
    }

    // A write-back the last session missed. Applied only if smoothwalker.ini still has the stamped write time.
    auto apply_pending_file_locked() -> void
    {
        auto content = dwsc::read_file(PENDING_PATH);
        if (!content) return;
        DeleteFileA(PENDING_PATH);

        std::optional<uint64_t> stamp;
        std::istringstream in(*content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos || dwsc::trim(line.substr(0, eq)) != "stamp") continue;
            try
            {
                stamp = std::stoull(dwsc::trim(line.substr(eq + 1)));
            }
            catch (...)
            {
            }
        }
        if (!stamp || *stamp != m_settings_stamp)
        {
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] smoothwalker.pending ignored: smoothwalker.ini changed since\n"));
            return;
        }
        auto numbers = dwsc::parse_numbers(*content);
        dwsc::apply_values(m_settings, dwsc::Values(numbers.begin(), numbers.end()));
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] applied {} settings the last session had not written yet\n"), numbers.size());
    }

    // Runs only while the camera is live, so no Mod Menu page is open to refuse its next Apply over the change.
    auto flush_locked(std::chrono::steady_clock::time_point now) -> void
    {
        // The file changed since last read (or is gone): let the poll handle it first, throttled so a deleted
        // file is not queried every tick.
        if (last_write(SETTINGS_PATH) != m_settings_stamp)
        {
            m_next_flush = now + std::chrono::milliseconds(250);
            return;
        }

        auto writes = pending_writes_locked();
        if (writes.empty())
        {
            mark_pending_locked();
            return;
        }

        auto content = dwsc::read_file(SETTINGS_PATH);
        auto updated = content ? dwsc::rewrite_numbers(*content, writes) : std::string{};
        bool changed = content && updated != *content;
        if (!content || (changed && !dwsc::write_file(SETTINGS_PATH, updated)))
        {
            if (!m_flush_failing) Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] could not write smoothwalker.ini, retrying\n"));
            m_flush_failing = true;
            m_next_flush = now + std::chrono::milliseconds(250);
            return;
        }
        if (changed) m_settings_stamp = last_write(SETTINGS_PATH);
        // A key missing from the file counts as written too, so it is not retried every tick.
        for (auto& [key, value] : writes) m_baseline[key] = std::stod(dwsc::format_number(value));
        m_flush_failing = false;
        mark_pending_locked(); // nothing left: clears the flag and deletes smoothwalker.pending
    }

    auto install_hook() -> bool
    {
        auto* camera = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__CameraComponent"));
        auto* rebel = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/RebelCamera.Default__RebelCameraComponent"));
        if (!camera || !rebel)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] camera class defaults not found, mod inactive\n"));
            return false;
        }
        auto** base = *reinterpret_cast<uintptr_t***>(camera);
        auto** vtable = *reinterpret_cast<uintptr_t***>(rebel);
        if (base[GET_CAMERA_VIEW_SLOT] == vtable[GET_CAMERA_VIEW_SLOT])
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] slot {} not overridden: unsupported game build, mod inactive\n"), GET_CAMERA_VIEW_SLOT);
            return false;
        }

        auto** entry = &vtable[GET_CAMERA_VIEW_SLOT];
        DWORD prev{};
        if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &prev))
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] vtable protect failed, mod inactive\n"));
            return false;
        }
        g_original = reinterpret_cast<GetCameraViewFn>(*entry);
        *entry = reinterpret_cast<uintptr_t*>(&get_camera_view_hook);
        VirtualProtect(entry, sizeof(*entry), prev, &prev);
        g_vtable_entry = entry;
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] GetCameraView hooked (slot {})\n"), GET_CAMERA_VIEW_SLOT);
        return true;
    }

    auto forget_player() -> void
    {
        g_player_camera.store(nullptr);
        g_player_root.store(nullptr);
        g_reset.store(true);
        m_controller = m_pawn = m_camera = m_root = {};
        m_player_known.store(false);
    }

    // A level change, called from the LoadMap pre hook and the engine tick; harmless if run twice for one load.
    auto forget_world() -> void
    {
        forget_player();
        m_tuner.forget();
        m_position_applied_generation = 0; // re-apply in the next world
    }

    // A duplicate of the new-object path where UE4SS installs BeginPlay. Game thread.
    auto on_begin_play(AActor* actor) -> void
    {
        auto* object = static_cast<UObject*>(actor);
        if (!object || m_controller.alive()) return;
        auto* cls = object->GetClassPrivate();
        if (cls && cls->GetNamePrivate() == m_player_controller_name) adopt_controller(dwsc::LiveRef::of(object));
    }

    auto on_end_play(AActor* actor) -> void
    {
        auto* object = static_cast<UObject*>(actor);
        if (object && object == m_controller.object) forget_player();
        else if (object && object == m_pawn.object) forget_pawn();
    }

    auto forget_pawn() -> void
    {
        g_player_camera.store(nullptr);
        g_player_root.store(nullptr);
        g_reset.store(true);
        m_pawn = m_camera = m_root = {};
    }

    auto adopt_controller(dwsc::LiveRef controller) -> void
    {
        forget_player();
        m_controller = controller;
        m_player_known.store(true);
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] player controller found\n"));
    }

    // GEngine->GameViewport->World, both reflected properties (not hard offsets); compared only, never followed.
    auto check_world(UEngine* engine) -> void
    {
        auto* object = static_cast<UObject*>(engine);
        if (!object || m_viewport_offset == -2 || m_world_offset == -2) return;
        auto offset_of = [](UObject* owner, const TCHAR* name) -> int32_t {
            auto** slot = owner->GetValuePtrByPropertyNameInChain<UObject*>(name);
            return slot ? static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(owner)) : -2;
        };
        if (m_viewport_offset < 0) m_viewport_offset = offset_of(object, STR("GameViewport"));
        if (m_viewport_offset < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] UEngine has no GameViewport property: level changes rely on the LoadMap hook\n"));
            return;
        }
        auto* viewport = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(object) + m_viewport_offset);
        UObject* world = nullptr;
        if (viewport)
        {
            if (m_world_offset < 0) m_world_offset = offset_of(viewport, STR("World"));
            if (m_world_offset < 0)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] GameViewportClient has no World property: level changes rely on the LoadMap hook\n"));
                return;
            }
            world = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(viewport) + m_world_offset);
        }
        if (m_world_seen && world == m_world) return;
        bool first = !m_world_seen;
        m_world_seen = true;
        m_world = world;
        if (first) return;
        forget_world();
        rescan_now(); // the new world's controller: look now if the new-object hand-off missed it
    }

    auto rescan_now() -> void
    {
        m_next_find = {};
        m_find_interval = std::chrono::seconds(2);
    }

    // The new-object hand-off (installed by every UE4SS profile) is primary; FindFirstOf is the fallback for a hot
    // reload or a missed hand-off, backing off per m_find_interval so the main menu is not scanned every tick.
    // Candidates are adopted only if the object array still holds them.
    auto discover_controller() -> void
    {
        if (m_new_controller_pending.exchange(false))
        {
            dwsc::LiveRef candidate;
            {
                std::lock_guard guard(m_new_controller_mutex);
                candidate = std::exchange(m_new_controller, {});
            }
            if (!m_controller.alive() && candidate.alive()) adopt_controller(candidate);
        }
        if (m_controller.object) return;

        auto now = std::chrono::steady_clock::now();
        bool requested = m_find_requested.exchange(false);
        if (!requested && now < m_next_find) return;
        m_next_find = now + m_find_interval;
        m_find_interval = std::min(m_find_interval * 2, std::chrono::seconds(60));
        auto candidate = dwsc::LiveRef::of(UObjectGlobals::FindFirstOf(STR("BP_PlayerController_C")));
        if (candidate.object && !candidate.object->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) &&
            candidate.alive())
        {
            adopt_controller(candidate);
        }
        else if (requested)
        {
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] player controller not found yet\n"));
        }
    }

    // Pointer reads and object array lookups only, before anything reads through a possibly-freed held pointer.
    auto on_engine_tick(UEngine* engine) -> void
    {
        if (dwapi::g_game_thread.load(std::memory_order_relaxed) == 0) dwapi::g_game_thread.store(GetCurrentThreadId());
        check_world(engine);
        if (m_controller.object && !m_controller.alive())
        {
            Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] player controller gone\n"));
            forget_player();
            rescan_now();
        }
        else if (m_pawn.object && !(m_pawn.alive() && (!m_camera.object || m_camera.alive()) && (!m_root.object || m_root.alive())))
        {
            forget_pawn();
        }
        discover_controller();

        show_pending_banner();
        apply_position();
        if (!m_controller.object) return;

        // Name lookup once per controller class, then a plain read.
        if (m_pawn_offset < 0)
        {
            auto** slot = m_controller.object->GetValuePtrByPropertyNameInChain<UObject*>(STR("Pawn"));
            if (!slot)
            {
                Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] controller has no Pawn property\n"));
                m_controller = {};
                m_player_known.store(false);
                return;
            }
            m_pawn_offset = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(m_controller.object));
        }
        auto* pawn = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(m_controller.object) + m_pawn_offset);
        if (pawn == m_pawn.object && !m_offset_retry) return;
        // The translation scan failed for this pawn (at the origin the triple matches twice): retry, backing off.
        if (pawn == m_pawn.object && std::chrono::steady_clock::now() < m_next_offset_scan) return;
        if (pawn != m_pawn.object) m_offset_wait = std::chrono::seconds(2); // a new pawn does not inherit the old one's wait
        m_offset_retry = false;
        forget_pawn();
        m_pawn = dwsc::LiveRef::of(pawn);
        // Controller.Pawn can point at a Garbage pawn until GC; re-adopting it would warn every tick.
        if (!m_pawn.alive()) return;

        auto camera = dwsc::LiveRef::of(object_ptr(pawn, STR("FollowCamera")));
        auto root = dwsc::LiveRef::of(object_ptr(pawn, STR("RootComponent")));
        // m_pawn stays set on failure, so a pawn without them is reported once, not every tick.
        if (!camera.alive() || !root.alive())
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] pawn {} has no FollowCamera or RootComponent\n"), pawn->GetName());
            return;
        }
        if (g_translation_offset.load() < 0 && !find_translation_offset(root.object))
        {
            m_offset_retry = true;
            m_next_offset_scan = std::chrono::steady_clock::now() + m_offset_wait;
            m_offset_wait = std::min(m_offset_wait * 2, std::chrono::seconds(60)); // each miss logs a warning
            return;
        }

        // The root is the capsule: its half height gives the feet point the vertical follow tracks.
        if (auto* half = root.object->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight")))
        {
            g_half_height_offset.store(static_cast<int32_t>(reinterpret_cast<uint8_t*>(half) - reinterpret_cast<uint8_t*>(root.object)));
        }
        else
        {
            g_half_height_offset.store(-1);
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] CapsuleHalfHeight not found: the vertical follow tracks the capsule centre\n"));
        }

        m_camera = camera;
        m_root = root;
        g_player_root.store(root.object);
        g_player_camera.store(camera.object);
        m_position_applied_generation = 0; // a new pawn: its modes get the current position
        m_tuner.camera_changed();
        m_offset_wait = std::chrono::seconds(2);
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] following {} (CapsuleHalfHeight at 0x{:X})\n"), pawn->GetName(), g_half_height_offset.load());
    }

    // ComponentToWorld is not reflected. A root's world translation equals its RelativeLocation, so the offset
    // is the one other place those doubles sit with a unit scale 0x20 after (UE 5.5 FTransform layout).
    auto find_translation_offset(UObject* root) -> bool
    {
        auto* relative = root->GetValuePtrByPropertyNameInChain<double>(STR("RelativeLocation"));
        if (!relative)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] RelativeLocation not found, smoothing inactive\n"));
            return false;
        }
        auto base = reinterpret_cast<uint8_t*>(root);
        auto relative_offset = reinterpret_cast<uint8_t*>(relative) - base;
        double want[3]{relative[0], relative[1], relative[2]};

        int32_t found = -1;
        int matches = 0;
        for (int32_t offset = 0x28; offset + 0x40 <= 0x800; offset += 8)
        {
            if (offset == relative_offset) continue;
            double block[7]{};
            if (!guarded_read(base + offset, block, sizeof(block))) break;
            bool location = std::abs(block[0] - want[0]) < 0.5 && std::abs(block[1] - want[1]) < 0.5 && std::abs(block[2] - want[2]) < 0.5;
            bool scale = std::abs(block[4] - 1.0) < 1e-3 && std::abs(block[5] - 1.0) < 1e-3 && std::abs(block[6] - 1.0) < 1e-3;
            if (location && scale)
            {
                found = offset;
                ++matches;
            }
        }
        if (matches != 1)
        {
            Output::send<LogLevel::Warning>(STR("[DWSmoothwalker] ComponentToWorld translation: {} matches, smoothing inactive\n"), matches);
            return false;
        }
        g_translation_offset.store(found);
        Output::send<LogLevel::Normal>(STR("[DWSmoothwalker] ComponentToWorld translation at 0x{:X} (RelativeLocation 0x{:X})\n"), found,
                                       relative_offset);
        return true;
    }
};

#define DW_SMOOTHWALKER_API __declspec(dllexport)
extern "C"
{
    DW_SMOOTHWALKER_API CppUserModBase* start_mod()
    {
        return new DWSmoothwalker();
    }

    DW_SMOOTHWALKER_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
