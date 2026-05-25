#include <cassert>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/simulation.h"
#include "world/public/world.h"

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::size_t count_events(
    const std::vector<KernelEvent>& events,
    KernelEventType type) {
    std::size_t count = 0;
    for (const KernelEvent& event : events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

network_example::NetId spawn_test_projectile(
    network_example::World& world,
    network_example::PeerId owner_peer,
    const glm::vec3& position,
    const glm::vec3& velocity,
    std::uint8_t weapon_id,
    std::uint32_t collision_mask = network_example::kCollisionLayerProjectile) {
    const network_example::NetId net_id =
        world.spawn_projectile(owner_peer, position, velocity);
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*entity);
    projectile.weapon_id = weapon_id;
    projectile.spawn_position = position;
    projectile.initial_velocity = velocity;
    projectile.previous_position = position;
    projectile.damage = 50;
    projectile.collision_mask = collision_mask;
    projectile.max_lifetime_seconds = 1.0f;
    return net_id;
}

void matching_projectiles_destroy_without_damage() {
    network_example::World world;
    const network_example::NetId lhs = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
        1);
    const network_example::NetId rhs = spawn_test_projectile(
        world,
        2,
        glm::vec3{1.0f, 0.5f, 0.0f},
        glm::vec3{-10.0f, 0.0f, 0.0f},
        2);

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 1;
    rule.rhs_weapon_id = 2;
    rule.symmetric = true;
    rule.destroy_lhs = true;
    rule.destroy_rhs = true;
    world.add_projectile_interaction_rule(rule);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 1, &events);

    require(!world.find_entity(lhs).has_value());
    require(!world.find_entity(rhs).has_value());
    require(count_events(events, KernelEventType_DamageApplied) == 0);
}

void matching_interaction_spawns_area_effect() {
    network_example::World world;
    spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
        3);
    spawn_test_projectile(
        world,
        2,
        glm::vec3{1.0f, 0.5f, 0.0f},
        glm::vec3{-10.0f, 0.0f, 0.0f},
        4);

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 3;
    rule.rhs_weapon_id = 4;
    rule.symmetric = true;
    rule.destroy_lhs = true;
    rule.destroy_rhs = true;
    rule.area_effect.enabled = true;
    rule.area_effect.radius = 2.5f;
    rule.area_effect.damage_interval_ticks = 3;
    rule.area_effect.lifetime_ticks = 7;
    rule.area_effect.damage_per_interval = 12;
    rule.area_effect.source_code = 9;
    rule.area_effect.collision_mask = network_example::kCollisionLayerEnemy;
    world.add_projectile_interaction_rule(rule);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 5, &events);

    require(count_events(events, KernelEventType_EntitySpawned) == 1);
    require(count_events(events, KernelEventType_DamageApplied) == 0);

    network_example::NetId area_effect_net_id = 0;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned) {
            area_effect_net_id = event.net_id;
            require(event.code ==
                    static_cast<std::uint32_t>(network_example::EntityType::kAreaEffect));
        }
    }
    const auto area_entity = world.find_entity(area_effect_net_id);
    require(area_entity.has_value());
    require(world.registry().all_of<network_example::AreaEffectState>(*area_entity));
    const network_example::AreaEffectState& area_effect =
        world.registry().get<network_example::AreaEffectState>(*area_entity);
    require(area_effect.radius == 2.5f);
    require(area_effect.damage_interval_ticks == 3);
    require(area_effect.expire_tick == 12);
    require(area_effect.damage_per_interval == 12);
    require(area_effect.source_code == 9);
    require(area_effect.collision_mask == network_example::kCollisionLayerEnemy);
}

void non_matching_weapon_ids_do_not_react() {
    network_example::World world;
    const network_example::NetId lhs = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
        1);
    const network_example::NetId rhs = spawn_test_projectile(
        world,
        2,
        glm::vec3{1.0f, 0.5f, 0.0f},
        glm::vec3{-10.0f, 0.0f, 0.0f},
        3);

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 1;
    rule.rhs_weapon_id = 2;
    rule.symmetric = true;
    world.add_projectile_interaction_rule(rule);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 1, &events);

    require(world.find_entity(lhs).has_value());
    require(world.find_entity(rhs).has_value());
    require(count_events(events, KernelEventType_DamageApplied) == 0);
}

void interaction_respects_masks_and_owner_peer_exclusion() {
    {
        network_example::World world;
        const network_example::NetId lhs = spawn_test_projectile(
            world,
            1,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{10.0f, 0.0f, 0.0f},
            1,
            network_example::kCollisionMaskDamageable);
        const network_example::NetId rhs = spawn_test_projectile(
            world,
            2,
            glm::vec3{1.0f, 0.5f, 0.0f},
            glm::vec3{-10.0f, 0.0f, 0.0f},
            2);
        network_example::ProjectileInteractionRule rule;
        rule.lhs_weapon_id = 1;
        rule.rhs_weapon_id = 2;
        rule.symmetric = true;
        world.add_projectile_interaction_rule(rule);

        std::vector<KernelEvent> events;
        network_example::simulate_projectiles(world, 0.05f, 1, &events);
        require(world.find_entity(lhs).has_value());
        require(world.find_entity(rhs).has_value());
    }

    {
        network_example::World world;
        const network_example::NetId lhs = spawn_test_projectile(
            world,
            7,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{10.0f, 0.0f, 0.0f},
            1);
        const network_example::NetId rhs = spawn_test_projectile(
            world,
            7,
            glm::vec3{1.0f, 0.5f, 0.0f},
            glm::vec3{-10.0f, 0.0f, 0.0f},
            2);
        network_example::ProjectileInteractionRule rule;
        rule.lhs_weapon_id = 1;
        rule.rhs_weapon_id = 2;
        rule.symmetric = true;
        world.add_projectile_interaction_rule(rule);

        std::vector<KernelEvent> events;
        network_example::simulate_projectiles(world, 0.05f, 1, &events);
        require(world.find_entity(lhs).has_value());
        require(world.find_entity(rhs).has_value());
    }
}

void multiple_reactions_resolve_by_projectile_pair_order() {
    network_example::World world;
    spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
        1);
    spawn_test_projectile(
        world,
        2,
        glm::vec3{1.0f, 0.5f, 0.0f},
        glm::vec3{-10.0f, 0.0f, 0.0f},
        2);
    spawn_test_projectile(
        world,
        3,
        glm::vec3{4.0f, 0.5f, 0.0f},
        glm::vec3{10.0f, 0.0f, 0.0f},
        1);
    spawn_test_projectile(
        world,
        4,
        glm::vec3{5.0f, 0.5f, 0.0f},
        glm::vec3{-10.0f, 0.0f, 0.0f},
        2);

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 1;
    rule.rhs_weapon_id = 2;
    rule.symmetric = true;
    rule.destroy_lhs = true;
    rule.destroy_rhs = true;
    rule.area_effect.enabled = true;
    rule.area_effect.radius = 1.0f;
    rule.area_effect.damage_interval_ticks = 1;
    rule.area_effect.lifetime_ticks = 3;
    world.add_projectile_interaction_rule(rule);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 1, &events);

    std::vector<network_example::NetId> area_effects;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned) {
            area_effects.push_back(event.net_id);
        }
    }
    require(area_effects.size() == 2);
    const auto first_area = world.find_entity(area_effects[0]);
    const auto second_area = world.find_entity(area_effects[1]);
    require(first_area.has_value());
    require(second_area.has_value());
    const glm::vec3 first_position =
        world.registry().get<network_example::Transform>(*first_area).position;
    const glm::vec3 second_position =
        world.registry().get<network_example::Transform>(*second_area).position;
    require(first_position.x < second_position.x);
}

}  // namespace

int main() {
    matching_projectiles_destroy_without_damage();
    matching_interaction_spawns_area_effect();
    non_matching_weapon_ids_do_not_react();
    interaction_respects_masks_and_owner_peer_exclusion();
    multiple_reactions_resolve_by_projectile_pair_order();
    return 0;
}
