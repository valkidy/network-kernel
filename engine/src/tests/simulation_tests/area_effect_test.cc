#include <cassert>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/simulation.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

network_example::Health& health(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::Health>(*entity);
}

network_example::NetId spawn_enemy(
    network_example::World& world,
    const glm::vec3& position) {
    const network_example::NetId enemy = world.spawn_enemy(position);
    health(world, enemy) = network_example::Health{50, 50};
    const auto entity = world.find_entity(enemy);
    assert(entity.has_value());
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};
    return enemy;
}

std::uint32_t count_damage_events(const std::vector<KernelEvent>& events) {
    std::uint32_t count = 0;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_DamageApplied) {
            ++count;
        }
    }
    return count;
}

void area_effect_damages_only_targets_inside_radius() {
    network_example::World world;
    const network_example::NetId inside = spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId outside = spawn_enemy(world, glm::vec3{5.0f, 0.0f, 0.0f});
    world.spawn_area_effect(0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    network_example::simulate_area_effects(world, 0, &events, &pipeline);
    pipeline.confirm_ready(world, 0, 0, &events);

    require(health(world, inside).hp == 30);
    require(health(world, outside).hp == 50);
    require(count_damage_events(events) == 1);
}

void area_effect_respects_per_target_damage_interval() {
    network_example::World world;
    const network_example::NetId target = spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    world.spawn_area_effect(0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    network_example::simulate_area_effects(world, 0, &events, &pipeline);
    pipeline.confirm_ready(world, 0, 0, &events);
    network_example::simulate_area_effects(world, 5, &events, &pipeline);
    pipeline.confirm_ready(world, 5, 5, &events);
    network_example::simulate_area_effects(world, 10, &events, &pipeline);
    pipeline.confirm_ready(world, 10, 10, &events);

    require(health(world, target).hp == 10);
    require(count_damage_events(events) == 2);
}

void server_owned_area_effect_uses_player_damage_grace() {
    network_example::World world;
    const network_example::NetId target =
        world.spawn_player(2, glm::vec3{1.0f, 0.0f, 0.0f});
    health(world, target) = network_example::Health{50, 50};
    const auto entity = world.find_entity(target);
    assert(entity.has_value());
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.5f, 0.0f}, {0.25f, 0.5f, 0.25f}, 0};
    world.spawn_area_effect(0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    network_example::simulate_area_effects(world, 12, 200000, &events, &pipeline);
    pipeline.confirm_ready(world, 200000, 12, &events);

    require(health(world, target).hp == 50);
    require(pipeline.pending_count() == 1);

    pipeline.confirm_ready(world, 300000, 18, &events);

    require(health(world, target).hp == 30);
    require(pipeline.pending_count() == 0);
}

void area_effect_expires_at_expire_tick() {
    network_example::World world;
    const network_example::NetId area =
        world.spawn_area_effect(0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 3, 20, 7);
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    network_example::simulate_area_effects(world, 3, &events, &pipeline);

    require(!world.find_entity(area).has_value());
    require(!events.empty());
    require(events.back().type == KernelEventType_EntityDestroyed);
    require(events.back().net_id == area);
}

void area_effect_damage_order_is_deterministic() {
    network_example::World world;
    const network_example::NetId first = spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.0f});
    const network_example::NetId second = spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    world.spawn_area_effect(0, glm::vec3{0.0f, 0.5f, 0.0f}, 3.0f, 1, 0, 10, 9);
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    network_example::simulate_area_effects(world, 0, &events, &pipeline);
    pipeline.confirm_ready(world, 0, 0, &events);

    require(events.size() == 4);
    require(events[0].type == KernelEventType_HitConfirmed);
    require(events[0].net_id == first);
    require(events[2].type == KernelEventType_HitConfirmed);
    require(events[2].net_id == second);
}

}  // namespace

int main() {
    area_effect_damages_only_targets_inside_radius();
    area_effect_respects_per_target_damage_interval();
    server_owned_area_effect_uses_player_damage_grace();
    area_effect_expires_at_expire_tick();
    area_effect_damage_order_is_deterministic();
    return 0;
}
