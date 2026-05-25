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

network_example::NetId spawn_player(
    network_example::World& world,
    network_example::PeerId owner_peer,
    const glm::vec3& position) {
    const network_example::NetId player = world.spawn_player(owner_peer, position);
    health(world, player) = network_example::Health{100, 100};
    return player;
}

void confirm_after_grace_window() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
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

void immediate_damage_request_applies_on_confirm() {
    network_example::World world;
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{0.0f, 0.0f, 0.0f});
    health(world, enemy) = network_example::Health{100, 100};
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    pipeline.submit_damage_request(network_example::DamageRequest{
        11,
        77,
        0,
        enemy,
        0,
        3,
        35,
        3000,
        glm::vec3{1.0f, 0.0f, 0.0f},
    });
    assert(health(world, enemy).hp == 100);

    pipeline.confirm_ready(world, 3000, 11, &events);
    assert(health(world, enemy).hp == 65);
    assert(events.size() == 2);
    assert(events[0].type == KernelEventType_HitConfirmed);
    assert(events[0].net_id == enemy);
    assert(events[0].code == 3);
    assert(events[1].type == KernelEventType_DamageApplied);
    assert(events[1].net_id == enemy);
    assert(events[1].code == 35);
}

void damage_requests_apply_in_deterministic_order() {
    network_example::World world;
    const network_example::NetId first =
        world.spawn_enemy(glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId second =
        world.spawn_enemy(glm::vec3{1.0f, 0.0f, 0.0f});
    health(world, first) = network_example::Health{100, 100};
    health(world, second) = network_example::Health{100, 100};
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    pipeline.submit_damage_request(network_example::DamageRequest{
        2,
        90,
        0,
        second,
        0,
        6,
        10,
        1000,
        glm::vec3{0.0f, 0.0f, 0.0f},
    });
    pipeline.submit_damage_request(network_example::DamageRequest{
        1,
        90,
        0,
        first,
        0,
        5,
        20,
        1000,
        glm::vec3{0.0f, 0.0f, 0.0f},
    });

    pipeline.confirm_ready(world, 1000, 3, &events);
    assert(events.size() == 4);
    assert(events[0].type == KernelEventType_HitConfirmed);
    assert(events[0].net_id == first);
    assert(events[1].type == KernelEventType_DamageApplied);
    assert(events[1].net_id == first);
    assert(events[2].type == KernelEventType_HitConfirmed);
    assert(events[2].net_id == second);
    assert(events[3].type == KernelEventType_DamageApplied);
    assert(events[3].net_id == second);
}

void dodge_cancels_pending_damage() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
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
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
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
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
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
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
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

void non_server_damage_applies_without_grace_window() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    assert(pipeline.submit_hit(world, player, 77, 2, 3, 40, 100000));
    assert(pipeline.pending_count() == 1);
    pipeline.confirm_ready(world, 100000, 6, &events);
    assert(health(world, player).hp == 60);
    assert(events.size() == 2);
}

}  // namespace

int main() {
    confirm_after_grace_window();
    immediate_damage_request_applies_on_confirm();
    damage_requests_apply_in_deterministic_order();
    dodge_cancels_pending_damage();
    parry_reduces_pending_damage();
    reload_does_not_modify_pending_damage();
    dodge_wins_over_parry();
    non_server_damage_applies_without_grace_window();
    return 0;
}
