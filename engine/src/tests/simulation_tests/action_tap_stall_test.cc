#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

namespace {

void require_impl(bool condition, int line) {
    if (!condition) {
        std::fprintf(
            stderr, "require failed at action_tap_stall_test.cc:%d\n", line);
        std::abort();
    }
}

#define require(condition) require_impl((condition), __LINE__)

/*
 * The real rifle_fire template from game_server/gameplay_catalog: a hold action
 * that reaches Active on the tick it is admitted, commits every third tick for
 * as long as it is held, and sits in Recovery for four ticks after it ends.
 */
network_example::RuntimeActionTemplate rifle_fire_template() {
    return network_example::RuntimeActionTemplate{
        1002u,
        KernelActionTriggerMode_Hold,
        static_cast<std::uint8_t>(
            KernelActionTemplateFlag_CancelOnRelease |
            KernelActionTemplateFlag_CancelOnDeath |
            KernelActionTemplateFlag_CancelOnWeaponChange |
            KernelActionTemplateFlag_CancelBeforeFirstCommit),
        1u,
        0u,
        3u,
        0u,
        4u,
        6u,
    };
}

network_example::NetId spawn_armed_player(network_example::World& world) {
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto entity = world.find_entity(player);
    assert(entity.has_value());
    world.registry().get<network_example::Health>(*entity) =
        network_example::Health{100, 100};

    const network_example::RuntimeActionTemplate fire = rifle_fire_template();
    network_example::WeaponTuning& tuning =
        world.registry().get_or_emplace<network_example::WeaponTuning>(*entity);
    tuning.configured = {true, false, false, false, false, false, false};
    network_example::WeaponMechanicsDefinition rifle;
    rifle.id = network_example::kWeaponSlot0;
    rifle.mode = network_example::WeaponFireMode::kHitscan;
    rifle.magazine_size = 3000;
    rifle.damage = 25;
    rifle.max_range = 100.0f;
    rifle.pellet_count = 1;
    rifle.fire_action_template_id = fire.action_template_id;
    rifle.reload_action_template_id = 2000u;
    tuning.definitions[network_example::kWeaponSlot0] = rifle;
    world.set_action_templates({fire});

    network_example::WeaponState& weapon =
        world.registry().get_or_emplace<network_example::WeaponState>(*entity);
    weapon.weapon_slot_count = 1;
    weapon.weapon_ids[0] = network_example::kWeaponSlot0;
    weapon.ammo[0] = rifle.magazine_size;
    weapon.reserve_magazines[0] = 3;
    return player;
}

std::vector<network_example::QueuedInput> queue(KernelPlayerInput input) {
    return {network_example::QueuedInput{1, input}};
}

KernelPlayerInput base_input() {
    KernelPlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.selected_weapon = network_example::kWeaponSlot0;
    return input;
}

/* The press frame: intent alone, exactly what the Unity sampler submits. */
KernelPlayerInput press_input(std::uint32_t action_instance_id) {
    KernelPlayerInput input = base_input();
    input.action_intent = KernelActionIntent{
        action_instance_id, KernelActionBinding_PrimaryFire, 0u, 0u};
    return input;
}

/* A later frame of the same press, or its release when held is 0. */
KernelPlayerInput hold_input(std::uint32_t action_instance_id, std::uint8_t held) {
    KernelPlayerInput input = base_input();
    input.action_input =
        KernelActionInput{action_instance_id, held, 0u, 0u};
    return input;
}

network_example::ActionRuntimeState& action_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::ActionRuntimeState>(*entity);
}

void step(
    network_example::World& world,
    const KernelPlayerInput& input,
    std::uint32_t tick,
    std::vector<network_example::ActionOutcome>* outcomes) {
    network_example::simulate_actions(world, queue(input), tick, outcomes);
}

/*
 * Tapping the trigger, the way a player does: press on one submission, release
 * on the next, repeat. Every tap is its own action instance, as the client
 * allocates one per press.
 *
 * The tap rate here is faster than the four recovery ticks the template asks
 * for, so some taps landing on Busy is correct and expected. What is not
 * expected is the actor never coming back: after the tapping stops, a press
 * with the whole recovery window ahead of it has to be admitted.
 */
void tapping_does_not_wedge_the_actor() {
    network_example::World world;
    const network_example::NetId player = spawn_armed_player(world);

    std::uint32_t tick = 100u;
    std::uint32_t action_instance_id = 1u;
    int admitted = 0;
    int busy = 0;

    for (int tap = 0; tap < 40; ++tap) {
        std::vector<network_example::ActionOutcome> outcomes;
        const std::uint32_t id = action_instance_id++;
        step(world, press_input(id), tick++, &outcomes);
        for (const network_example::ActionOutcome& outcome : outcomes) {
            if (outcome.type == network_example::ActionOutcomeType::Admitted) {
                ++admitted;
            } else if (
                outcome.reason == KernelLocalActionResultReason_Busy) {
                ++busy;
            }
        }

        // The release lands on the next submission, as it does in the client.
        std::vector<network_example::ActionOutcome> release_outcomes;
        step(world, hold_input(id, 0u), tick++, &release_outcomes);
    }

    std::fprintf(
        stderr,
        "tapping: admitted=%d busy=%d phase=%d instance=%u\n",
        admitted,
        busy,
        static_cast<int>(action_state(world, player).phase),
        action_state(world, player).action_instance_id);

    // Let every recovery window drain with no input at all.
    for (int idle = 0; idle < 30; ++idle) {
        std::vector<network_example::ActionOutcome> outcomes;
        network_example::simulate_actions(world, {}, tick++, &outcomes);
    }

    std::fprintf(
        stderr,
        "after idle: phase=%d instance=%u\n",
        static_cast<int>(action_state(world, player).phase),
        action_state(world, player).action_instance_id);

    // A clean press, with nothing in its way, must fire.
    std::vector<network_example::ActionOutcome> final_outcomes;
    step(world, press_input(action_instance_id), tick, &final_outcomes);
    bool final_admitted = false;
    for (const network_example::ActionOutcome& outcome : final_outcomes) {
        if (outcome.type == network_example::ActionOutcomeType::Admitted) {
            final_admitted = true;
        } else {
            std::fprintf(
                stderr,
                "final press refused: reason=%d\n",
                static_cast<int>(outcome.reason));
        }
    }

    require(final_admitted);
}

/*
 * Prediction replays ticks it has already simulated. advance_action early-outs
 * whenever last_advanced_tick already equals the tick being simulated, and
 * reset_action deliberately carries last_advanced_tick across a reset, so a tick
 * that is simulated twice advances the action only once. This walks that path
 * directly: every tick is submitted twice, as a rollback that replays the
 * present tick would.
 */
void a_tick_simulated_twice_does_not_wedge_the_actor() {
    network_example::World world;
    const network_example::NetId player = spawn_armed_player(world);

    std::uint32_t tick = 100u;
    std::uint32_t action_instance_id = 1u;

    for (int tap = 0; tap < 40; ++tap) {
        const std::uint32_t id = action_instance_id++;
        std::vector<network_example::ActionOutcome> outcomes;
        step(world, press_input(id), tick, &outcomes);
        step(world, press_input(id), tick, &outcomes);
        ++tick;
        std::vector<network_example::ActionOutcome> release_outcomes;
        step(world, hold_input(id, 0u), tick, &release_outcomes);
        step(world, hold_input(id, 0u), tick, &release_outcomes);
        ++tick;
    }

    std::fprintf(
        stderr,
        "twice-per-tick: phase=%d instance=%u\n",
        static_cast<int>(action_state(world, player).phase),
        action_state(world, player).action_instance_id);

    for (int idle = 0; idle < 30; ++idle) {
        std::vector<network_example::ActionOutcome> outcomes;
        network_example::simulate_actions(world, {}, tick++, &outcomes);
    }

    std::vector<network_example::ActionOutcome> final_outcomes;
    step(world, press_input(action_instance_id), tick, &final_outcomes);
    bool final_admitted = false;
    for (const network_example::ActionOutcome& outcome : final_outcomes) {
        if (outcome.type == network_example::ActionOutcomeType::Admitted) {
            final_admitted = true;
        } else {
            std::fprintf(
                stderr,
                "twice-per-tick final press refused: reason=%d phase=%d\n",
                static_cast<int>(outcome.reason),
                static_cast<int>(action_state(world, player).phase));
        }
    }

    require(final_admitted);
}

/*
 * The same shape a rollback produces: simulate forward, then replay the last few
 * ticks from an older one, repeatedly.
 */
void replayed_ticks_do_not_wedge_the_actor() {
    network_example::World world;
    const network_example::NetId player = spawn_armed_player(world);

    std::uint32_t tick = 100u;
    std::uint32_t action_instance_id = 1u;

    for (int tap = 0; tap < 30; ++tap) {
        const std::uint32_t id = action_instance_id++;
        std::vector<network_example::ActionOutcome> outcomes;
        step(world, press_input(id), tick, &outcomes);
        step(world, hold_input(id, 0u), tick + 1u, &outcomes);
        // Roll back two ticks and replay them.
        step(world, press_input(id), tick, &outcomes);
        step(world, hold_input(id, 0u), tick + 1u, &outcomes);
        tick += 2u;
    }

    std::fprintf(
        stderr,
        "replayed: phase=%d instance=%u\n",
        static_cast<int>(action_state(world, player).phase),
        action_state(world, player).action_instance_id);

    for (int idle = 0; idle < 30; ++idle) {
        std::vector<network_example::ActionOutcome> outcomes;
        network_example::simulate_actions(world, {}, tick++, &outcomes);
    }

    std::vector<network_example::ActionOutcome> final_outcomes;
    step(world, press_input(action_instance_id), tick, &final_outcomes);
    bool final_admitted = false;
    for (const network_example::ActionOutcome& outcome : final_outcomes) {
        if (outcome.type == network_example::ActionOutcomeType::Admitted) {
            final_admitted = true;
        } else {
            std::fprintf(
                stderr,
                "replayed final press refused: reason=%d phase=%d\n",
                static_cast<int>(outcome.reason),
                static_cast<int>(action_state(world, player).phase));
        }
    }

    require(final_admitted);
}

}  // namespace

int main() {
    tapping_does_not_wedge_the_actor();
    a_tick_simulated_twice_does_not_wedge_the_actor();
    replayed_ticks_do_not_wedge_the_actor();
    std::fprintf(stderr, "action_tap_stall_test passed\n");
    return 0;
}
