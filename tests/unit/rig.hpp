// The camera core under test: a Pipeline and its Authority on a clock the test sets (10 MHz ticks that move only
// when the test moves them), a GuardedCopy that fails the access the test chose, a player root and camera the hook
// reads, and a stub processor whose output the test sets.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "camera/authority.hpp"
#include "camera/pipeline.hpp"
#include "common/math.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rig
{
    inline int64_t ticks = 1'000'000'000; // the clock now
    constexpr double FREQUENCY = 10'000'000.0;
    inline auto now() -> int64_t { return ticks; }
    inline auto frequency() -> double { return FREQUENCY; }
    constexpr dw::camera::Clock CLOCK{&now, &frequency};
    inline auto advance(double seconds) -> void { ticks += static_cast<int64_t>(std::llround(seconds * FREQUENCY)); }

    // Guarded accesses, counted from 1 since the last arm(); the armed one fails.
    inline int copies = 0;
    inline int fail_at = 0;
    inline auto arm(int nth) -> void
    {
        copies = 0;
        fail_at = nth;
    }
    inline auto copy(void* dst, const void* src, size_t n) -> bool
    {
        if (++copies == fail_at) return false;
        std::memcpy(dst, src, n);
        return true;
    }

    // FMinimalViewInfo's head as the hook reads it, and bytes past it the hook must never touch.
    struct View
    {
        dw::camera::ViewHead head;
        uint8_t tail[16];
    };

    constexpr int32_t TRANSLATION = 0x200;
    constexpr int32_t HALF_HEIGHT = 0x100;

    // Processor output: the game's camera moved by `offset` and its FOV by `fov_add`, with the generation and transition
    // the test sets.
    class Stub final : public dw::camera::Processor
    {
      public:
        bool on = true;
        uint64_t toggle = 0;
        uint64_t generation = 0;
        double transition = 0.0;
        dw::Vec3 offset{};
        double fov_add = 0.0;
        bool wall_clamp = false;
        dw::camera::FrameIn last{};
        int calls = 0;

        auto frame(const dw::camera::FrameIn& in, dw::camera::FrameOut& out) -> void override
        {
            last = in;
            ++calls;
            out.location = in.camera + offset;
            out.rotation = in.rotation;
            out.fov_add = fov_add;
            out.generation = generation;
            out.transition = transition;
            out.wall_clamp = wall_clamp;
        }
        auto state(uint64_t& toggle_generation) const -> bool override
        {
            toggle_generation = toggle;
            return on;
        }
    };

    struct Core
    {
        dw::camera::Authority authority{pipeline, CLOCK};
        dw::camera::Pipeline pipeline{authority, CLOCK, &copy};
        uint8_t root[0x300]{}; // read through memcpy, so any alignment
        int camera_object = 0; // the player camera's identity: only its address is compared
        View game{}; // what the game's own GetCameraView writes, every update
        View view{}; // what the hook left: the game's view, then the hook's write

        Core()
        {
            arm(0);
            pipeline.set_player_camera(&camera_object);
            pipeline.set_player_root(root);
            pipeline.set_translation_offset(TRANSLATION);
            pipeline.set_half_height_offset(HALF_HEIGHT);
            set_pivot({0, 0, 100}, 96.0f);
            set_game_view({-300, 0, 180});
        }

        auto set_pivot(dw::Vec3 p, float half_height) -> void
        {
            double xyz[3]{p.x, p.y, p.z};
            std::memcpy(root + TRANSLATION, xyz, sizeof(xyz));
            std::memcpy(root + HALF_HEIGHT, &half_height, sizeof(half_height));
        }

        // The game's view for this update, as its own GetCameraView leaves it.
        auto set_game_view(dw::Vec3 location, double pitch = 0, double yaw = 0, double roll = 0, float fov = 90.0f) -> void
        {
            game = View{};
            game.head = {{location.x, location.y, location.z}, {pitch, yaw, roll}, fov};
            std::memset(game.tail, 0xAB, sizeof(game.tail));
        }

        // One player-camera update: the clock moves `seconds`, the game writes its view, then the hook runs.
        auto update(float delta_time = 0.125f, double seconds = 0.125) -> const dw::camera::ViewHead&
        {
            advance(seconds);
            view = game;
            pipeline.on_camera_view(&camera_object, delta_time, &view);
            return view.head;
        }

        // The hook left the game's view as it was, to the byte.
        auto untouched() const -> bool { return std::memcmp(&view, &game, sizeof(View)) == 0; }

        auto location() const -> dw::Vec3 { return {view.head.location[0], view.head.location[1], view.head.location[2]}; }
    };
} // namespace rig
