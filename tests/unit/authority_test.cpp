// camera/authority: consumers, claim and its answers, lease renewal and expiry, release, the 8 layer slots, re-keys
// and uninstalls, and apply_layers on its own.

#include "check.hpp"
#include "rig.hpp"

#include <string>

using dw::camera::ApiResult;
using dw::camera::Layer;
using dw::camera::Snap;

namespace
{
    int keys[10]; // consumer keys: only their addresses count

    struct Fixture
    {
        rig::Core core;
        rig::Stub stub; // on, so an update runs the whole pipeline and takes a pending cut
        dw::camera::Authority& a = core.authority;
        Fixture()
        {
            core.pipeline.register_processor(stub);
            for (int i = 0; i < 3; ++i) a.install(&keys[i], "mod" + std::to_string(i), "1.0");
        }
        auto cut_pending() -> bool { return core.pipeline.inspect().reset && core.pipeline.inspect().reset_reason == static_cast<int>(Snap::ApiCut); }
        auto clear_cut() -> void
        {
            core.update(); // the hook takes the pending cut
            CHECK(!core.pipeline.inspect().reset);
        }
    };

    auto layer(double up) -> Layer
    {
        Layer l;
        l.offset[2] = up;
        return l;
    }
} // namespace

TEST(authority, install_returns_the_version)
{
    rig::Core c;
    CHECK_EQ(c.authority.install(&keys[0], "a", "0.10.1"), std::string("0.10.1"));
    CHECK_EQ(c.authority.consumers().size(), 1u);
    std::string mod;
    bool first = false;
    CHECK_EQ(c.authority.register_consumer(&keys[0], mod, first), ApiResult::Ok);
    CHECK(first);
    CHECK_EQ(mod, std::string("a"));
    CHECK_EQ(c.authority.register_consumer(&keys[0], mod, first), ApiResult::Ok);
    CHECK(!first);
    CHECK_EQ(c.authority.register_consumer(&keys[5], mod, first), ApiResult::UnknownState);
}

TEST(authority, claim_results)
{
    Fixture f;
    CHECK_EQ(f.a.claim(&keys[9], false, 0.0), ApiResult::UnknownState);
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::Ok);
    CHECK_EQ(f.a.owner(nullptr), std::string("mod0"));
    CHECK_EQ(f.a.inspect().owner_slot, 0);
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::AlreadyYours);
    CHECK_EQ(f.a.claim(&keys[1], false, 0.0), ApiResult::Taken); // first come, no priorities
    CHECK_EQ(f.a.release(&keys[1], true), ApiResult::NotOwner);
    CHECK_EQ(f.a.release(&keys[9], true), ApiResult::UnknownState);
    CHECK_EQ(f.a.release(&keys[0], true), ApiResult::Ok);
    CHECK_EQ(f.a.owner(nullptr), std::string());
    CHECK_EQ(f.a.inspect().owner_slot, -1);
    CHECK_EQ(f.a.claim(&keys[1], false, 0.0), ApiResult::Ok);
}

TEST(authority, claim_must_keep_while_a_live_fade_runs)
{
    Fixture f;
    f.core.update(); // publishes the view snapshot
    f.a.set_blending(true);
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::MustKeep);
    rig::advance(0.2499); // still live
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::MustKeep);
    rig::advance(0.0001); // the camera stopped updating: the flag is not trusted
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::Ok);
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::AlreadyYours); // the owner is never refused
}

TEST(authority, must_keep_needs_a_snapshot)
{
    Fixture f;
    f.a.set_blending(true); // no update yet, so no snapshot: nothing is live
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::Ok);
}

TEST(authority, release_cut_and_glide)
{
    Fixture f;
    f.clear_cut();
    const uint64_t g = f.a.inspect().release_generation;
    f.a.claim(&keys[0], false, 0.0);
    f.a.release(&keys[0], true);
    CHECK_EQ(f.a.inspect().release_generation, g + 1);
    CHECK(!f.core.pipeline.inspect().reset);
    f.a.claim(&keys[0], false, 0.0);
    f.a.release(&keys[0], false);
    CHECK_EQ(f.a.inspect().release_generation, g + 1);
    CHECK(f.cut_pending());
}

TEST(authority, lease_renewal)
{
    Fixture f;
    const int64_t t0 = rig::ticks;
    CHECK_EQ(f.a.claim(&keys[0], false, 1.0), ApiResult::Ok);
    CHECK_EQ(f.a.inspect().owner_expires, t0 + 10'000'000);
    rig::advance(0.5);
    CHECK_EQ(f.a.claim(&keys[0], true, 2.0), ApiResult::AlreadyYours); // a ttl renews, from now
    CHECK_EQ(f.a.inspect().owner_expires, rig::ticks + 20'000'000);
    CHECK(f.a.inspect().owner_keep_layers);
    const int64_t renewed = f.a.inspect().owner_expires;
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::AlreadyYours); // a bare claim is a probe: nothing changes
    CHECK_EQ(f.a.inspect().owner_expires, renewed);
    CHECK(f.a.inspect().owner_keep_layers);
    int64_t expires = 0;
    CHECK_EQ(f.a.owner(&expires), std::string("mod0"));
    CHECK_EQ(expires, renewed);
}

TEST(authority, lease_expiry)
{
    Fixture f;
    f.clear_cut();
    f.a.claim(&keys[0], false, 1.0);
    rig::advance(0.999);
    CHECK_EQ(f.a.owner(nullptr), std::string("mod0"));
    rig::advance(0.001);
    CHECK_EQ(f.a.owner(nullptr), std::string()); // reads as nobody at once
    CHECK_EQ(f.a.inspect().owner_slot, 0);       // dropped when a claim or release next looks
    CHECK(!f.core.pipeline.inspect().reset);
    CHECK_EQ(f.a.release(&keys[0], true), ApiResult::NotOwner); // the expired owner is nobody
    CHECK_EQ(f.a.inspect().owner_slot, -1);
    CHECK(f.cut_pending()); // the drop is a release("cut"), whatever the call asked
}

TEST(authority, expired_claim_is_free)
{
    Fixture f;
    f.a.claim(&keys[0], false, 0.25);
    rig::advance(0.25);
    CHECK_EQ(f.a.claim(&keys[1], false, 0.0), ApiResult::Ok);
    CHECK_EQ(f.a.owner(nullptr), std::string("mod1"));
    CHECK_EQ(f.a.inspect().owner_expires, 0);
}

TEST(authority, eight_slots_then_none)
{
    rig::Core c;
    for (int i = 0; i < 10; ++i) c.authority.install(&keys[i], "mod" + std::to_string(i), "1");
    for (int i = 0; i < 8; ++i) CHECK_EQ(c.authority.layer_set(&keys[i], layer(1), 0.0), ApiResult::Ok);
    CHECK_EQ(c.authority.layer_set(&keys[8], layer(1), 0.0), ApiResult::NoSlot);
    CHECK_EQ(c.authority.layer_set(&keys[0], layer(2), 0.0), ApiResult::Ok); // its own slot again
    CHECK_EQ(c.authority.layer_clear(&keys[1]), ApiResult::Ok);                // a cleared layer keeps its slot
    CHECK_EQ(c.authority.layer_set(&keys[8], layer(1), 0.0), ApiResult::NoSlot);
    CHECK_EQ(c.authority.layer_set(&keys[9], layer(1), 0.0), ApiResult::NoSlot);
    CHECK_EQ(c.authority.layer_set(&keys[5], layer(1), 0.0), ApiResult::Ok);
    CHECK_EQ(c.authority.layer_clear(&keys[7]), ApiResult::Ok);
    int unknown = 0;
    CHECK_EQ(c.authority.layer_clear(&unknown), ApiResult::UnknownState);
    CHECK_EQ(c.authority.layer_set(&unknown, layer(1), 0.0), ApiResult::UnknownState);
}

TEST(authority, rekey_frees_the_slot)
{
    rig::Core c;
    for (int i = 0; i < 9; ++i) c.authority.install(&keys[i], "mod" + std::to_string(i), "1");
    for (int i = 0; i < 8; ++i) c.authority.layer_set(&keys[i], layer(1), 0.0);
    CHECK_EQ(c.authority.layer_set(&keys[8], layer(1), 0.0), ApiResult::NoSlot);
    c.authority.install(&keys[3], "mod3", "1"); // a Lua restart on the same key, without a stop
    Layer layers[dw::camera::MAX_LAYERS];
    std::string owners[dw::camera::MAX_LAYERS];
    c.authority.layers(layers, owners);
    CHECK(owners[3].empty());
    CHECK(!layers[3].active); // fading out
    CHECK_EQ(c.authority.layer_set(&keys[8], layer(1), 0.0), ApiResult::Ok);
    c.authority.layers(layers, owners);
    CHECK_EQ(owners[3], std::string("mod8"));
    CHECK_EQ(c.authority.consumers().size(), 9u);
}

TEST(authority, rekey_of_the_owner_releases_with_a_cut)
{
    Fixture f;
    f.clear_cut();
    f.a.claim(&keys[0], false, 0.0);
    f.a.install(&keys[0], "mod0", "1.0");
    CHECK_EQ(f.a.owner(nullptr), std::string());
    CHECK(f.cut_pending());
    CHECK_EQ(f.a.claim(&keys[1], false, 0.0), ApiResult::Ok);
}

TEST(authority, uninstall_releases_with_a_cut)
{
    Fixture f;
    f.clear_cut();
    f.a.claim(&keys[0], false, 0.0);
    f.a.layer_set(&keys[0], layer(5), 0.0);
    f.a.uninstall(&keys[0]);
    CHECK(f.cut_pending());
    CHECK_EQ(f.a.inspect().owner_slot, -1);
    CHECK_EQ(f.a.consumers().size(), 2u);
    Layer layers[dw::camera::MAX_LAYERS];
    std::string owners[dw::camera::MAX_LAYERS];
    f.a.layers(layers, owners);
    CHECK(owners[0].empty());
    CHECK(!layers[0].active);
    CHECK_EQ(f.a.claim(&keys[0], false, 0.0), ApiResult::UnknownState);
    f.a.uninstall(&keys[9]); // unknown: nothing happens
    CHECK_EQ(f.a.consumers().size(), 2u);
}

TEST(authority, uninstall_of_another_keeps_the_owner)
{
    Fixture f;
    f.clear_cut();
    f.a.claim(&keys[0], false, 0.0);
    f.a.uninstall(&keys[1]);
    CHECK_EQ(f.a.owner(nullptr), std::string("mod0"));
    CHECK(!f.core.pipeline.inspect().reset);
}

TEST(authority, layer_set_clamps)
{
    Fixture f;
    Layer l;
    l.weight = 3.0;
    l.blend = -1.0;
    l.fov_abs = 500.0;
    f.a.layer_set(&keys[0], l, 0.5);
    Layer layers[dw::camera::MAX_LAYERS];
    std::string owners[dw::camera::MAX_LAYERS];
    f.a.layers(layers, owners);
    CHECK_EQ(layers[0].weight, 1.0);
    CHECK_EQ(layers[0].blend, 0.0);
    CHECK_EQ(layers[0].fov_abs, 170.0);
    CHECK(layers[0].active);
    CHECK_EQ(layers[0].expires, rig::ticks + 5'000'000);
    CHECK_EQ(owners[0], std::string("mod0"));
}

TEST(authority, apply_layers_sums_and_weights)
{
    Fixture f;
    Layer a = layer(10);
    a.weight = 0.5;
    a.rotation[1] = 20.0;
    Layer b = layer(4);
    b.fov_abs = 60.0; // absolute FOV, blended in by weight
    f.a.layer_set(&keys[0], a, 0.0);
    f.a.layer_set(&keys[1], b, 0.0);
    double location[3]{0, 0, 0}, rotation[3]{0, 0, 0};
    float fov = 90.0f;
    CHECK(f.a.apply_layers(location, rotation, fov, 0.1));
    CHECK_NEAR(location[2], 5.0 + 4.0, 1e-12);
    CHECK_NEAR(rotation[1], 10.0, 1e-12);
    CHECK_EQ(fov, 60.0f);
}

TEST(authority, apply_layers_idle)
{
    Fixture f;
    double location[3]{}, rotation[3]{};
    float fov = 90.0f;
    CHECK(!f.a.apply_layers(location, rotation, fov, 0.1)); // none set
    f.a.layer_set(&keys[0], layer(10), 0.0);
    CHECK(f.a.apply_layers(location, rotation, fov, 0.1));
    f.a.layer_clear(&keys[0]); // blend 0: gone in one step, then idle
    location[2] = 0.0;
    CHECK(!f.a.apply_layers(location, rotation, fov, 0.1));
    CHECK_EQ(location[2], 0.0);
    CHECK(!f.a.layers_any());
    Layer zero; // an active layer that moves nothing still counts as set
    f.a.layer_set(&keys[0], zero, 0.0);
    CHECK(f.a.apply_layers(location, rotation, fov, 0.1));
    CHECK_EQ(location[2], 0.0);
    CHECK(f.a.layers_any());
}
