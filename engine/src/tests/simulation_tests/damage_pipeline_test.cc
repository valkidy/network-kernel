#include <cassert>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/simulation.h"

namespace {

network_example::Health& health(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::Health>(*entity);
}

PlayerInput defensive_input(std::uint32_t buttons, std::uint64_t action_time_us) {
    PlayerInput input{};
    input.input_seq = 1;
    input.client_action_time_us = action_time_us;
    input.buttons = buttons;
    return input;
}

void confirm_after_grace_window() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    assert(pipeline.submit_hit(world, player, 77, 0, 3, 40, 1000));
    assert(health(world, player).hp == 100);
    assert(pipeline.pending_count() == 1);
    pipeline.confirm_ready(world, 100999, 3, &events);
    assert(health(world, player).hp == 100);
    assert(events.empty());

    pipeline.confirm_ready(world, 101000, 4, &events);
    assert(health(world, player).hp == 60);
    assert(pipeline.pending_count() == 0);
    assert(events.size() == 2);
    assert(events[0].type == KernelEventType_HitConfirmed);
    assert(events[1].type == KernelEventType_DamageApplied);
    assert(events[1].code == 40);
    assert(events[0].event_time_us == 1000);
    assert(events[0].presentation_time_us == 1000);
    assert(events[1].event_time_us == 1000);
    assert(events[1].presentation_time_us == 1000);
}

void dodge_cancels_pending_damage() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    assert(pipeline.submit_hit(world, player, 77, 0, 3, 40, 100000));
    pipeline.ingest_defensive_input(
        1,
        defensive_input(InputButton_Dodge, 90000),
        120000);
    pipeline.confirm_ready(world, 200000, 6, &events);
    assert(health(world, player).hp == 100);
    assert(events.empty());
}

void parry_reduces_pending_damage() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    assert(pipeline.submit_hit(world, player, 77, 0, 3, 41, 100000));
    pipeline.ingest_defensive_input(
        1,
        defensive_input(InputButton_Parry, 90000),
        120000);
    pipeline.confirm_ready(world, 200000, 6, &events);
    assert(health(world, player).hp == 79);
    assert(events.size() == 2);
    assert(events[1].code == 21);
}

void reload_does_not_modify_pending_damage() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    assert(pipeline.submit_hit(world, player, 77, 0, 3, 40, 100000));
    pipeline.ingest_defensive_input(
        1,
        defensive_input(InputButton_Reload, 90000),
        120000);
    pipeline.confirm_ready(world, 200000, 6, &events);
    assert(health(world, player).hp == 60);
    assert(events.size() == 2);
    assert(events[1].code == 40);
}

void dodge_wins_over_parry() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    assert(pipeline.submit_hit(world, player, 77, 0, 3, 40, 100000));
    pipeline.ingest_defensive_input(
        1,
        defensive_input(InputButton_Dodge | InputButton_Parry, 90000),
        120000);
    pipeline.confirm_ready(world, 200000, 6, &events);
    assert(health(world, player).hp == 100);
    assert(events.empty());
}

void non_server_damage_is_not_pended() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;

    assert(!pipeline.submit_hit(world, player, 77, 2, 3, 40, 100000));
    assert(pipeline.pending_count() == 0);
}

}  // namespace

int main() {
    confirm_after_grace_window();
    dodge_cancels_pending_damage();
    parry_reduces_pending_damage();
    reload_does_not_modify_pending_damage();
    dodge_wins_over_parry();
    non_server_damage_is_not_pended();
    return 0;
}
