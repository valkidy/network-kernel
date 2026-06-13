#include <cassert>
#include <cmath>
#include <cstdlib>
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

network_example::Transform& transform(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::Transform>(*entity);
}

network_example::ProjectileState& projectile_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::ProjectileState>(*entity);
}

network_example::HomingState& homing_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::HomingState>(*entity);
}

network_example::NetId spawned_projectile(const std::vector<KernelEvent>& events) {
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned &&
            event.code == static_cast<std::uint32_t>(network_example::EntityType::kProjectile)) {
            return event.net_id;
        }
    }
    return 0;
}

network_example::WeaponMechanicsDefinition homing_weapon_definition() {
    network_example::WeaponMechanicsDefinition definition{};
    definition.id = network_example::kWeaponSlot6;
    definition.mode = network_example::WeaponFireMode::kProjectile;
    definition.magazine_size = 4;
    definition.damage = 20;
    definition.cooldown_ticks = 1;
    definition.reload_ticks = 30;
    definition.projectile_speed = 5.0f;
    definition.projectile_lifetime_seconds = 3.0f;
    definition.projectile_motion_model = network_example::ProjectileMotionModel::kHoming;
    definition.projectile_hit_response = network_example::ProjectileHitResponse::kDestroy;
    definition.projectile_damage_shape = network_example::ProjectileDamageShape::kDirectHit;
    definition.projectile_collision_mask = network_example::kCollisionLayerEnemy;
    definition.projectile_max_hit_count = 1;
    definition.homing_mode = network_example::HomingMode::kFireAndForget;
    definition.homing_sync_mode =
        network_example::ProjectileSyncMode::kHybridDeterministicThenSnapshot;
    definition.homing_boost_ticks = 1;
    definition.homing_lock_on_range = 20.0f;
    definition.homing_lose_target_range = 25.0f;
    definition.homing_lock_cone_degrees = 75.0f;
    definition.homing_max_turn_rate_degrees_per_second = 720.0f;
    definition.homing_acceleration = 30.0f;
    definition.homing_max_speed = 10.0f;
    return definition;
}

void configure_homing_weapon(
    network_example::World& world,
    network_example::NetId player) {
    const auto entity = world.find_entity(player);
    assert(entity.has_value());
    network_example::WeaponTuning& tuning =
        world.registry().get_or_emplace<network_example::WeaponTuning>(*entity);
    tuning.configured[network_example::kWeaponSlot6] = true;
    tuning.definitions[network_example::kWeaponSlot6] = homing_weapon_definition();

    network_example::WeaponState& weapon =
        world.registry().get_or_emplace<network_example::WeaponState>(*entity);
    weapon.ammo[network_example::kWeaponSlot6] = 4;
    weapon.reserve_ammo[network_example::kWeaponSlot6] = 4;
}

network_example::NetId spawn_player(
    network_example::World& world,
    network_example::PeerId owner_peer,
    const glm::vec3& position) {
    const network_example::NetId player = world.spawn_player(owner_peer, position);
    health(world, player) = network_example::Health{100, 100};
    const auto entity = world.find_entity(player);
    assert(entity.has_value());
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0};
    configure_homing_weapon(world, player);
    return player;
}

network_example::NetId spawn_enemy(
    network_example::World& world,
    const glm::vec3& position) {
    const network_example::NetId enemy = world.spawn_enemy(position);
    health(world, enemy) = network_example::Health{60, 60};
    const auto entity = world.find_entity(enemy);
    assert(entity.has_value());
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.8f, 0.0f}, {0.4f, 0.8f, 0.4f}, 0};
    return enemy;
}

PlayerInput homing_fire_input(std::uint32_t client_action_id) {
    PlayerInput input{};
    input.input_seq = 1;
    input.client_action_id = client_action_id;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire;
    input.selected_weapon = network_example::kWeaponSlot6;
    return input;
}

void homing_projectile_spawns_and_boosts_deterministically() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    spawn_enemy(world, glm::vec3{8.0f, 0.0f, 3.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        {network_example::QueuedInput{1, homing_fire_input(7001)}},
        0,
        &events);

    const network_example::NetId projectile = spawned_projectile(events);
    assert(projectile != 0);
    assert(projectile_state(world, projectile).client_action_id == 7001);
    assert(projectile_state(world, projectile).motion_model ==
           network_example::ProjectileMotionModel::kHoming);
    assert(homing_state(world, projectile).phase ==
           network_example::MissileGuidancePhase::kBoost);

    network_example::simulate_projectiles(world, 0.1f, 1, &events);
    const glm::vec3 boosted_position = transform(world, projectile).position;
    assert(boosted_position.x > 0.49f && boosted_position.x < 0.51f);
    assert(std::fabs(boosted_position.z) < 0.001f);
}

void homing_projectile_guides_to_target_and_applies_damage() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{3.0f, 0.0f, 1.0f});

    std::vector<KernelEvent> events;
    network_example::DamagePipeline pipeline;
    network_example::simulate_weapons(
        world,
        {network_example::QueuedInput{1, homing_fire_input(7002)}},
        0,
        &events);
    const network_example::NetId projectile = spawned_projectile(events);
    assert(projectile != 0);

    for (std::uint32_t tick = 1; tick <= 20 && world.find_entity(projectile).has_value();
         ++tick) {
        network_example::simulate_projectiles(world, 0.1f, tick, &events, &pipeline);
        pipeline.confirm_ready(
            world,
            static_cast<std::uint64_t>(tick) * 100000u,
            tick,
            &events);
    }

    assert(health(world, enemy).hp < 60);
    assert(!world.find_entity(projectile).has_value());
}

void homing_projectile_selects_targets_deterministically() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId farther =
        spawn_enemy(world, glm::vec3{8.0f, 0.2f, 0.0f});
    const network_example::NetId nearer =
        spawn_enemy(world, glm::vec3{4.0f, 0.2f, 0.0f});
    assert(nearer > farther);

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        {network_example::QueuedInput{1, homing_fire_input(7003)}},
        0,
        &events);
    const network_example::NetId projectile = spawned_projectile(events);
    assert(projectile != 0);

    network_example::simulate_projectiles(world, 0.1f, 1, &events);
    network_example::simulate_projectiles(world, 0.1f, 2, &events);

    assert(homing_state(world, projectile).phase ==
           network_example::MissileGuidancePhase::kGuided);
    assert(homing_state(world, projectile).target_net_id == nearer);
}

void homing_projectile_loses_invalid_target_without_retargeting() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId first =
        spawn_enemy(world, glm::vec3{4.0f, 0.2f, 0.0f});
    spawn_enemy(world, glm::vec3{5.0f, 0.2f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        {network_example::QueuedInput{1, homing_fire_input(7004)}},
        0,
        &events);
    const network_example::NetId projectile = spawned_projectile(events);
    assert(projectile != 0);
    network_example::simulate_projectiles(world, 0.1f, 1, &events);
    network_example::simulate_projectiles(world, 0.1f, 2, &events);
    assert(homing_state(world, projectile).target_net_id == first);

    assert(world.destroy(first));
    network_example::simulate_projectiles(world, 0.1f, 3, &events);

    assert(homing_state(world, projectile).phase ==
           network_example::MissileGuidancePhase::kLostTarget);
    assert(homing_state(world, projectile).target_net_id == 0);
}

}  // namespace

int main() {
    homing_projectile_spawns_and_boosts_deterministically();
    homing_projectile_guides_to_target_and_applies_damage();
    homing_projectile_selects_targets_deterministically();
    homing_projectile_loses_invalid_target_without_retargeting();
    return 0;
}
