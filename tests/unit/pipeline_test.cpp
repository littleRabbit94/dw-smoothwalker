// camera/pipeline: one player-camera update through the hook. Cuts and their reasons, the off-and-settled path, the
// crossfade, ownership and its falling edges, layers through the hook, guarded accesses, bad views and delta times,
// hook timing, camera_live and the API snapshot. Deterministic: the test's clock and guarded copy (rig.hpp).

#include "check.hpp"
#include "rig.hpp"

#include <cmath>
#include <string>

using dw::camera::Snap;

namespace
{
    auto snap(const rig::Core& c) -> Snap { return static_cast<Snap>(c.pipeline.read_debug().snap); }

    struct Fixture
    {
        rig::Core core;
        rig::Stub stub;
        Fixture()
        {
            CHECK(core.pipeline.register_processor(stub));
            core.update(); // startup: the first update is a cut
        }
    };

    // smoothstep, as the crossfade eases
    auto eased(double s) -> double { return s * s * (3.0 - 2.0 * s); }

    int keys[4]; // consumer keys: only their addresses count
} // namespace

TEST(pipeline, startup_is_a_cut)
{
    rig::Core c;
    rig::Stub s;
    CHECK(c.pipeline.register_processor(s));
    CHECK(!c.pipeline.register_processor(s)); // one slot
    c.update();
    CHECK_EQ(snap(c), Snap::Startup);
    CHECK(s.last.restart);
    CHECK(s.last.enabled);
    CHECK_EQ(c.pipeline.read_debug().snap_age, 0.0);
    rig::advance(1.0);
    CHECK_NEAR(c.pipeline.read_debug().snap_age, 1.0, 1e-12);
}

TEST(pipeline, frame_in)
{
    Fixture f;
    f.core.set_pivot({10, 20, 130}, 42.0f);
    f.core.set_game_view({-290, 20, 210}, -10, 30, 0);
    f.core.update();
    const auto& in = f.stub.last;
    CHECK(!in.restart);
    CHECK_EQ(in.dt, 0.1); // the rig's 0.125 s delta, clamped
    CHECK_EQ(in.pivot.x, 10.0);
    CHECK_EQ(in.pivot.z, 130.0);
    CHECK_EQ(in.half_height, 42.0);
    CHECK_EQ(in.camera.x, -290.0);
    CHECK(dw::angle_between(in.rotation, dw::from_rotator(-10, 30, 0)) < 1e-6);
}

TEST(pipeline, gap)
{
    Fixture f;
    f.core.update(0.125f, 0.25); // exactly reset_gap: not a gap
    CHECK(!f.stub.last.restart);
    f.core.update(0.125f, 0.2500001);
    CHECK(f.stub.last.restart);
    CHECK_EQ(snap(f.core), Snap::Gap);
}

TEST(pipeline, teleport)
{
    Fixture f;
    f.core.set_pivot({500, 0, 100}, 96.0f); // exactly reset_distance: not a teleport
    f.core.update();
    CHECK(!f.stub.last.restart);
    f.core.set_pivot({1000.001, 0, 100}, 96.0f);
    f.core.update();
    CHECK(f.stub.last.restart);
    CHECK_EQ(snap(f.core), Snap::Teleport);
    f.core.set_pivot({NAN, 0, 100}, 96.0f); // a non-finite pivot loses the view rather than teleporting
    f.core.update();
    CHECK(f.core.untouched());
}

TEST(pipeline, cut_thresholds)
{
    Fixture f;
    f.core.pipeline.set_cut_thresholds(100.0, 1.0);
    f.core.update(0.125f, 0.5); // under the new gap
    CHECK(!f.stub.last.restart);
    f.core.set_pivot({150, 0, 100}, 96.0f); // over the new distance
    f.core.update();
    CHECK_EQ(snap(f.core), Snap::Teleport);
    f.core.pipeline.set_cut_thresholds(NAN, 0.1); // ignored
    f.core.update(0.125f, 0.5);
    CHECK(!f.stub.last.restart);
}

TEST(pipeline, requested_cut_keeps_its_first_reason)
{
    Fixture f;
    f.core.pipeline.request_cut(Snap::World);
    f.core.pipeline.request_cut(Snap::Player); // a level change is followed by a new controller: World stays
    f.core.update();
    CHECK(f.stub.last.restart);
    CHECK_EQ(snap(f.core), Snap::World);
    f.core.pipeline.request_cut(Snap::Pawn); // the hook took the last one: a new reason
    f.core.update();
    CHECK_EQ(snap(f.core), Snap::Pawn);
}

TEST(pipeline, cut_reason_precedence)
{
    // A requested cut beats the hook's own gap check.
    {
        Fixture f;
        f.core.pipeline.request_cut(Snap::Pawn);
        f.core.update(0.125f, 1.0);
        CHECK_EQ(snap(f.core), Snap::Pawn);
    }
    // Why the follow was dropped beats a gap.
    {
        Fixture f;
        rig::arm(1); // the pivot read faults
        f.core.update();
        rig::arm(0);
        f.core.update(0.125f, 1.0);
        CHECK_EQ(snap(f.core), Snap::ViewLost);
    }
    // A requested cut beats why the follow was dropped.
    {
        Fixture f;
        rig::arm(1);
        f.core.update();
        rig::arm(0);
        f.core.pipeline.request_cut(Snap::World);
        f.core.update();
        CHECK_EQ(snap(f.core), Snap::World);
    }
    // Switched back on beats a cut requested while off.
    {
        Fixture f;
        f.stub.on = false;
        f.core.update();
        f.core.pipeline.request_cut(Snap::World);
        f.stub.on = true;
        ++f.stub.toggle;
        f.core.update();
        CHECK(f.stub.last.restart);
        CHECK_EQ(snap(f.core), Snap::Toggle);
    }
}

TEST(pipeline, off_and_settled_leaves_the_view)
{
    rig::Core c;
    rig::Stub s;
    s.on = false;
    CHECK(c.pipeline.register_processor(s));
    s.offset = {0, 50, 0};
    c.update();
    CHECK(c.untouched());
    CHECK_EQ(s.calls, 0); // returns before any processor call
    CHECK(!c.pipeline.processor_enabled());
    dw::camera::Snapshot snapshot{};
    CHECK(c.pipeline.snapshot().read(snapshot));
    CHECK_EQ(snapshot.shown.location[0], -300.0);
    CHECK_EQ(snapshot.game.location[2], 180.0);
    CHECK(std::isnan(snapshot.pivot[0])); // no pivot read on this path
    CHECK_EQ(snapshot.qpc, rig::ticks);
    CHECK(std::isnan(c.pipeline.read_debug().lag_h));
}

TEST(pipeline, no_processor_leaves_the_view)
{
    rig::Core c;
    c.update();
    CHECK(c.untouched());
    c.update();
    CHECK(c.untouched());
    CHECK(!c.authority.enabled());
    CHECK_EQ(c.pipeline.view_updates(), 2u);
}

TEST(pipeline, other_cameras_are_ignored)
{
    Fixture f;
    int other = 0;
    rig::View v = f.core.game;
    f.core.pipeline.on_camera_view(&other, 0.125f, &v);
    CHECK(std::memcmp(&v, &f.core.game, sizeof(v)) == 0);
    CHECK_EQ(f.core.pipeline.view_updates(), 1u);
    CHECK_EQ(f.stub.calls, 1);
}

TEST(pipeline, processor_moves_the_view)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.core.update();
    CHECK_NEAR(f.core.location().y, 50.0, 1e-12);
    CHECK_NEAR(f.core.pipeline.read_debug().lag_h, 50.0, 1e-9);
    CHECK_EQ(f.core.view.head.fov, 90.0f);
    for (uint8_t b : f.core.view.tail) CHECK_EQ(b, 0xAB); // only the head is written
    dw::camera::Snapshot snapshot{};
    CHECK(f.core.pipeline.snapshot().read(snapshot));
    CHECK_NEAR(snapshot.shown.location[1], 50.0, 1e-12);
    CHECK_EQ(snapshot.game.location[1], 0.0);
    CHECK_EQ(snapshot.pivot[2], 100.0);
}

// The processor's FOV add (the speed blend): on the game's FOV, before the layers, faded like any change, clamped, and
// neither on an implausible FOV nor while another mod owns the camera.
TEST(pipeline, processor_fov_add)
{
    Fixture f;
    f.stub.fov_add = 6.0;
    f.core.update();
    CHECK_EQ(f.stub.last.fov, 90.0);
    CHECK_EQ(f.core.view.head.fov, 96.0f);
    dw::camera::Snapshot snapshot{};
    CHECK(f.core.pipeline.snapshot().read(snapshot));
    CHECK_EQ(snapshot.shown.fov, 96.0);
    CHECK_EQ(snapshot.game.fov, 90.0);

    // A layer's FOV goes on top.
    f.core.authority.install(&keys[0], "Zoom", "1");
    dw::camera::Layer l;
    l.fov_delta = 10.0;
    f.core.authority.layer_set(&keys[0], l, 0.0);
    f.core.update();
    CHECK_EQ(f.core.view.head.fov, 106.0f);
    f.core.authority.uninstall(&keys[0]);
    f.core.update();
    CHECK_EQ(f.core.view.head.fov, 96.0f);

    // A change with the generation fades from the FOV shown.
    f.stub.transition = 0.4;
    f.stub.fov_add = 0.0;
    ++f.stub.generation;
    f.core.update();
    CHECK_NEAR(f.core.view.head.fov, 96.0 - 6.0 * eased(0.25), 1e-4);
    for (int i = 0; i < 4; ++i) f.core.update();
    CHECK_EQ(f.core.view.head.fov, 90.0f);

    // Clamped; nothing on an implausible FOV, which the processor reads as NAN.
    f.stub.transition = 0.0;
    f.stub.fov_add = 200.0;
    f.core.update();
    CHECK_EQ(f.core.view.head.fov, 170.0f);
    f.core.set_game_view({-300, 0, 180}, 0, 0, 0, 0.5f);
    f.core.update();
    CHECK(std::isnan(f.stub.last.fov));
    CHECK_EQ(f.core.view.head.fov, 0.5f);

    // Owned: the game's view, FOV included.
    f.core.set_game_view({-300, 0, 180});
    f.stub.fov_add = 6.0;
    f.core.authority.install(&keys[1], "Photo", "1");
    CHECK_EQ(f.core.authority.claim(&keys[1], false, 0.0), dw::camera::ApiResult::Ok);
    f.core.update();
    CHECK(f.core.untouched());
}

// A generation change fades from the last shown offset to the new one over the processor's transition, eased with a
// smoothstep on the world delta: offset A + (B - A) * smoothstep(elapsed / transition). 0.1 s steps (the rig's
// 0.125 s delta, clamped) over 0.4 s.
TEST(pipeline, crossfade_endpoints)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.stub.transition = 0.4;
    f.core.update();
    f.stub.offset = {0, -50, 0};
    ++f.stub.generation;
    for (int i = 1; i <= 4; ++i)
    {
        f.core.update();
        CHECK_NEAR(f.core.location().y, 50.0 - 100.0 * eased(i * 0.25), 1e-9);
        CHECK_EQ(f.core.authority.inspect().blending, i < 4);
    }
    f.core.update();
    CHECK_NEAR(f.core.location().y, -50.0, 1e-9);
    CHECK(!f.stub.last.restart); // a fade, never a cut
}

TEST(pipeline, transition_zero_snaps_to_the_change)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.core.update();
    f.stub.offset = {0, -50, 0};
    ++f.stub.generation; // transition 0: no fade
    f.core.update();
    CHECK_NEAR(f.core.location().y, -50.0, 1e-9);
    CHECK(!f.core.authority.inspect().blending);

    f.stub.transition = 0.5;
    f.stub.offset = {0, 50, 0};
    ++f.stub.generation;
    f.core.update();
    CHECK(f.core.authority.inspect().blending);
    f.stub.transition = 0.0; // set to 0 mid-fade: the fade stops
    f.core.update();
    CHECK(!f.core.authority.inspect().blending);
    CHECK_NEAR(f.core.location().y, 50.0, 1e-9);
}

TEST(pipeline, crossfade_triggers)
{
    Fixture f;
    f.stub.transition = 0.4;
    f.core.update();
    f.core.pipeline.set_cut_thresholds(500.0, 0.25); // unchanged values: no generation, no fade
    f.core.update();
    CHECK(!f.core.authority.inspect().blending);
    f.core.pipeline.set_cut_thresholds(600.0, 0.25);
    f.core.update();
    CHECK(f.core.authority.inspect().blending);

    Fixture g;
    g.stub.transition = 0.4;
    g.stub.offset = {0, 50, 0};
    g.core.update();
    g.stub.on = false; // the toggle: fades to the game's view, then leaves it alone
    ++g.stub.toggle;
    g.core.update();
    CHECK(g.core.authority.inspect().blending);
    CHECK_NEAR(g.core.location().y, 50.0 - 50.0 * eased(0.25), 1e-9);
    for (int i = 0; i < 3; ++i) g.core.update();
    CHECK_NEAR(g.core.location().y, 0.0, 1e-9);
    const int calls = g.stub.calls;
    g.core.update();
    CHECK(g.core.untouched());
    CHECK_EQ(g.stub.calls, calls);
}

TEST(pipeline, a_cut_ends_a_fade)
{
    Fixture f;
    f.stub.transition = 0.5;
    f.stub.offset = {0, 50, 0};
    f.core.update();
    ++f.stub.generation;
    f.stub.offset = {0, -50, 0};
    f.core.update();
    CHECK(f.core.authority.inspect().blending);
    f.core.pipeline.request_cut(Snap::Pawn);
    f.core.update();
    CHECK(!f.core.authority.inspect().blending);
    CHECK_NEAR(f.core.location().y, -50.0, 1e-9);
}

TEST(pipeline, owned_camera_shows_the_game_view)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.core.authority.install(&keys[0], "Photo", "1");
    CHECK_EQ(f.core.authority.claim(&keys[0], false, 0.0), dw::camera::ApiResult::Ok);
    const int calls = f.stub.calls;
    f.core.update();
    CHECK(f.core.untouched());
    CHECK_EQ(f.stub.calls, calls + 1); // the processor still runs, warm
    CHECK(!f.stub.last.restart);
}

TEST(pipeline, owner_keeps_layers_only_when_asked)
{
    Fixture f;
    f.core.authority.install(&keys[0], "Photo", "1");
    f.core.authority.install(&keys[1], "Shake", "1");
    dw::camera::Layer l;
    l.offset[2] = 10.0;
    CHECK_EQ(f.core.authority.layer_set(&keys[1], l, 0.0), dw::camera::ApiResult::Ok);
    CHECK_EQ(f.core.authority.claim(&keys[0], false, 0.0), dw::camera::ApiResult::Ok);
    f.core.update();
    CHECK(f.core.untouched());
    CHECK_EQ(f.core.authority.claim(&keys[0], true, 5.0), dw::camera::ApiResult::AlreadyYours); // renewal takes keep_layers
    f.core.update();
    CHECK_NEAR(f.core.location().z, 190.0, 1e-9);
    CHECK_NEAR(f.core.location().y, 0.0, 1e-9);
}

TEST(pipeline, release_glide_fades_back)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.stub.transition = 0.4;
    f.core.authority.install(&keys[0], "Photo", "1");
    f.core.authority.claim(&keys[0], false, 0.0);
    f.core.update();
    f.core.update();
    CHECK_EQ(f.core.authority.release(&keys[0], true), dw::camera::ApiResult::Ok);
    f.core.update();
    CHECK(!f.stub.last.restart);
    CHECK(f.core.pipeline.read_debug().glide);
    CHECK_NEAR(f.core.location().y, 50.0 * eased(0.25), 1e-9); // from the game's view as the owner left it
    for (int i = 0; i < 3; ++i) f.core.update();
    CHECK_NEAR(f.core.location().y, 50.0, 1e-9);
    CHECK(!f.core.pipeline.read_debug().glide);
}

TEST(pipeline, release_cut_snaps_back)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.core.authority.install(&keys[0], "Photo", "1");
    f.core.authority.claim(&keys[0], false, 0.0);
    f.core.update();
    CHECK_EQ(f.core.authority.release(&keys[0], false), dw::camera::ApiResult::Ok);
    f.core.update();
    CHECK(f.stub.last.restart);
    CHECK_EQ(snap(f.core), Snap::ApiCut);
    CHECK_NEAR(f.core.location().y, 50.0, 1e-9);
}

TEST(pipeline, lease_end_is_a_cut)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.core.authority.install(&keys[0], "Photo", "1");
    f.core.authority.claim(&keys[0], false, 0.3);
    f.core.update(); // 0.125 s in
    f.core.update(); // 0.25
    CHECK(f.core.untouched());
    CHECK_EQ(f.core.authority.owner(nullptr), std::string("Photo"));
    f.core.update(); // 0.375: the hook sees the lease out before the game thread drops the claim
    CHECK(f.stub.last.restart);
    CHECK_EQ(snap(f.core), Snap::ClaimEnded);
    CHECK_NEAR(f.core.location().y, 50.0, 1e-9);
    CHECK_EQ(f.core.authority.owner(nullptr), std::string()); // an expired lease reads as nobody
    CHECK_EQ(f.core.authority.inspect().owner_slot, 0);       // dropped only when the game thread next looks
}

TEST(pipeline, uninstall_of_the_owner_is_a_cut)
{
    Fixture f;
    f.core.authority.install(&keys[0], "Photo", "1");
    f.core.authority.claim(&keys[0], false, 0.0);
    f.core.update();
    f.core.authority.uninstall(&keys[0]);
    f.core.update();
    CHECK_EQ(snap(f.core), Snap::ApiCut);
}

TEST(pipeline, layer_through_the_hook_with_the_processor_off)
{
    rig::Core c;
    c.authority.install(&keys[0], "Shake", "1");
    dw::camera::Layer l;
    l.offset[0] = 100.0; // forward, camera frame
    l.fov_delta = 10.0;
    c.authority.layer_set(&keys[0], l, 0.0);
    c.set_game_view({-300, 0, 180}, 0, 90, 0); // looking along +Y
    c.update(0.0625f, 0.0625);
    CHECK_NEAR(c.location().x, -300.0, 1e-9);
    CHECK_NEAR(c.location().y, 100.0, 1e-9);
    CHECK_EQ(c.view.head.fov, 100.0f);
}

TEST(pipeline, layer_blends_expires_and_idles)
{
    rig::Core c;
    c.authority.install(&keys[0], "Shake", "1");
    dw::camera::Layer l;
    l.offset[2] = 100.0;
    l.blend = 0.125;
    c.authority.layer_set(&keys[0], l, 0.15);
    c.update(0.0625f, 0.0625);
    CHECK_NEAR(c.location().z, 180.0 + 50.0, 1e-9); // half way through its blend
    c.update(0.0625f, 0.0625);
    CHECK_NEAR(c.location().z, 180.0 + 100.0, 1e-9);
    c.update(0.0625f, 0.0625); // past the ttl: fades out as if cleared, over its last blend
    CHECK_NEAR(c.location().z, 180.0 + 50.0, 1e-9);
    c.update(0.0625f, 0.0625); // faded out: idle, nothing written
    CHECK(c.untouched());
    CHECK(!c.authority.inspect().layers_any);
    c.update(0.0625f, 0.0625); // off and settled again
    CHECK(c.untouched());
}

TEST(pipeline, layer_steps_on_the_clamped_delta)
{
    rig::Core c;
    c.authority.install(&keys[0], "Shake", "1");
    dw::camera::Layer l;
    l.offset[2] = 100.0;
    l.blend = 0.4;
    c.authority.layer_set(&keys[0], l, 0.0);
    c.update(0.25f, 0.2); // the world delta is clamped to 0.1 s: a quarter of the blend
    CHECK_NEAR(c.location().z, 180.0 + 100.0 * eased(0.25), 1e-9);
}

TEST(pipeline, guarded_access_faults)
{
    // Accesses in order: the pivot, the view, the half height, the write.
    for (int nth : {1, 2, 4})
    {
        Fixture f;
        f.stub.offset = {0, 50, 0};
        rig::arm(nth);
        f.core.update();
        CHECK(f.core.untouched());
        CHECK(std::isnan(f.core.pipeline.read_debug().lag_h));
        CHECK(std::isnan(f.core.pipeline.read_debug().keep_follow));
        rig::arm(0);
        f.core.update();
        CHECK(f.stub.last.restart);
        CHECK_EQ(snap(f.core), Snap::ViewLost);
        CHECK_NEAR(f.core.location().y, 50.0, 1e-9);
    }
    Fixture f;
    f.stub.offset = {0, 50, 0};
    rig::arm(3); // the half height: unknown, the update goes on
    f.core.update();
    CHECK(std::isnan(f.stub.last.half_height));
    CHECK(!f.stub.last.restart);
    CHECK_NEAR(f.core.location().y, 50.0, 1e-9);
}

TEST(pipeline, implausible_half_height)
{
    Fixture f;
    f.core.set_pivot({0, 0, 100}, 1500.0f);
    f.core.update();
    CHECK(std::isnan(f.stub.last.half_height));
    f.core.set_pivot({0, 0, 100}, -1.0f);
    f.core.update();
    CHECK(std::isnan(f.stub.last.half_height));
    f.core.pipeline.set_half_height_offset(-1); // not resolved: not read, so the third access is the write
    rig::arm(3);
    f.core.update();
    CHECK(std::isnan(f.stub.last.half_height));
    CHECK(f.core.untouched());
}

TEST(pipeline, nan_view_is_left_alone)
{
    Fixture f;
    f.stub.offset = {0, 50, 0};
    f.core.set_game_view({NAN, 0, 180});
    f.core.update();
    CHECK(f.core.untouched());
    f.core.set_game_view({-300, 0, 180}, 0, INFINITY, 0);
    f.core.update();
    CHECK(f.core.untouched());
    f.core.set_game_view({-300, 0, 180});
    f.core.update();
    CHECK_EQ(snap(f.core), Snap::ViewLost);
    CHECK_NEAR(f.core.location().y, 50.0, 1e-9);

    f.stub.offset = {NAN, 0, 0}; // a non-finite result: the game's view stays
    f.core.update();
    CHECK(f.core.untouched());
}

TEST(pipeline, odd_delta_time)
{
    Fixture f;
    const double seconds = f.core.pipeline.view_seconds();
    CHECK_EQ(seconds, 0.125);
    f.core.update(-1.0f);
    CHECK_EQ(f.stub.last.dt, 0.0);
    f.core.update(0.0f);
    CHECK_EQ(f.stub.last.dt, 0.0);
    f.core.update(INFINITY);
    CHECK_EQ(f.stub.last.dt, 0.1);
    f.core.update(1.0f);
    CHECK_EQ(f.stub.last.dt, 0.1);
    f.core.update(NAN);
    CHECK(std::isnan(f.stub.last.dt)); // std::clamp passes a NaN through to the processor
    CHECK_EQ(f.core.pipeline.view_seconds(), seconds + 1.0); // only the finite positive ones count
    CHECK_EQ(f.core.pipeline.view_updates(), 6u);
}

TEST(pipeline, camera_live)
{
    rig::Core c;
    CHECK(!c.pipeline.camera_live()); // no update yet
    c.update();
    CHECK(c.pipeline.camera_live());
    rig::advance(0.2499);
    CHECK(c.pipeline.camera_live());
    rig::advance(0.0001);
    CHECK(!c.pipeline.camera_live());
}

TEST(pipeline, hook_timing)
{
    Fixture f;
    uint64_t calls = 99;
    double us = -1;
    f.core.pipeline.take_hook_timing(calls, us);
    CHECK_EQ(calls, 0u); // off by default
    CHECK_EQ(us, 0.0);
    f.core.pipeline.set_diagnostics(dw::camera::DIAG_HOOK_TIMING);
    f.core.update();
    f.core.update();
    f.core.pipeline.take_hook_timing(calls, us);
    CHECK_EQ(calls, 2u);
    CHECK_EQ(us, 0.0); // the test clock stands still inside an update
    f.core.pipeline.take_hook_timing(calls, us);
    CHECK_EQ(calls, 0u);
    f.core.pipeline.set_diagnostics(0);
}

TEST(pipeline, unregister_and_listener_slots)
{
    rig::Core c;
    rig::Stub s, other;
    CHECK(c.pipeline.register_processor(s));
    CHECK(c.pipeline.unregister_processor(other)); // not the registered one: nothing to wait for
    CHECK(c.pipeline.unregister_processor(s));
    CHECK(c.pipeline.register_processor(other));
    c.update();
    CHECK_EQ(s.calls, 0);
    CHECK_EQ(other.calls, 1);
}
