#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
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

network_example::WeaponState& weapon_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::WeaponState>(*entity);
}

network_example::ProjectileState& projectile_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::ProjectileState>(*entity);
}

network_example::Transform& transform_state(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    return world.registry().get<network_example::Transform>(*entity);
}

PlayerInput fire_input(std::uint8_t weapon_id) {
    PlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire;
    input.selected_weapon = weapon_id;
    return input;
}

std::vector<network_example::QueuedInput> queue(PlayerInput input) {
    return {network_example::QueuedInput{1, input}};
}

network_example::WeaponMechanicsDefinition weapon_definition(
    std::uint8_t id,
    network_example::WeaponFireMode mode,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t cooldown_ticks,
    std::uint32_t reload_ticks,
    float max_range = 0.0f) {
    network_example::WeaponMechanicsDefinition definition;
    definition.id = id;
    definition.mode = mode;
    definition.magazine_size = magazine_size;
    definition.damage = damage;
    definition.cooldown_ticks = cooldown_ticks;
    definition.reload_ticks = reload_ticks;
    definition.max_range = max_range;
    definition.pellet_count = 1;
    return definition;
}

void configure_test_weapons(
    network_example::World& world,
    network_example::NetId net_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    network_example::WeaponTuning& tuning =
        world.registry().get_or_emplace<network_example::WeaponTuning>(*entity);
    tuning.configured = {true, true, true, true, true, false, false};
    tuning.definitions = {{
        weapon_definition(
            network_example::kWeaponSlot0,
            network_example::WeaponFireMode::kHitscan,
            30,
            25,
            3,
            30,
            100.0f),
        weapon_definition(
            network_example::kWeaponSlot1,
            network_example::WeaponFireMode::kShotgun,
            8,
            10,
            20,
            45,
            40.0f),
        weapon_definition(
            network_example::kWeaponSlot2,
            network_example::WeaponFireMode::kProjectile,
            30,
            40,
            30,
            60),
        weapon_definition(
            network_example::kWeaponSlot3,
            network_example::WeaponFireMode::kProjectile,
            6,
            45,
            45,
            75),
        weapon_definition(
            network_example::kWeaponSlot4,
            network_example::WeaponFireMode::kAreaEffect,
            3,
            12,
            10,
            30),
    }};
    tuning.definitions[network_example::kWeaponSlot1].pellet_count = 5;
    tuning.definitions[network_example::kWeaponSlot1].pellet_spread = 0.035f;
    tuning.definitions[network_example::kWeaponSlot2].projectile_speed = 15.0f;
    tuning.definitions[network_example::kWeaponSlot2].projectile_lifetime_seconds = 3.0f;
    tuning.definitions[network_example::kWeaponSlot2].explosion_radius = 4.0f;
    tuning.definitions[network_example::kWeaponSlot2].projectile_motion_model =
        network_example::ProjectileMotionModel::kParabolic;
    tuning.definitions[network_example::kWeaponSlot2].projectile_gravity =
        glm::vec3{0.0f, -9.8f, 0.0f};
    tuning.definitions[network_example::kWeaponSlot3].projectile_speed = 35.0f;
    tuning.definitions[network_example::kWeaponSlot3].projectile_lifetime_seconds = 2.5f;
    tuning.definitions[network_example::kWeaponSlot3].explosion_radius = 3.0f;
    tuning.definitions[network_example::kWeaponSlot4].area_effect_radius = 2.0f;
    tuning.definitions[network_example::kWeaponSlot4].area_effect_damage_per_interval = 12;
    tuning.definitions[network_example::kWeaponSlot4].area_effect_damage_interval_ticks = 2;
    tuning.definitions[network_example::kWeaponSlot4].area_effect_lifetime_ticks = 6;
    tuning.definitions[network_example::kWeaponSlot4].area_effect_spawn_distance = 1.0f;
    tuning.definitions[network_example::kWeaponSlot4].area_effect_collision_mask =
        network_example::kCollisionLayerEnemy;

    network_example::WeaponState& weapon =
        world.registry().get_or_emplace<network_example::WeaponState>(*entity);
    for (std::size_t index = 0; index < network_example::kWeaponCount; ++index) {
        weapon.ammo[index] = tuning.definitions[index].magazine_size;
        weapon.reserve_ammo[index] =
            static_cast<std::uint16_t>(tuning.definitions[index].magazine_size * 3u);
    }
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
    configure_test_weapons(world, player);
    return player;
}

network_example::NetId spawn_enemy(
    network_example::World& world,
    const glm::vec3& position) {
    const network_example::NetId enemy = world.spawn_enemy(position);
    health(world, enemy) = network_example::Health{50, 50};
    const auto entity = world.find_entity(enemy);
    assert(entity.has_value());
    world.registry().get<network_example::Hitbox>(*entity) =
        network_example::Hitbox{{0.0f, 0.8f, 0.0f}, {0.4f, 0.8f, 0.4f}, 0};
    return enemy;
}

std::uint32_t count_events(
    const std::vector<KernelEvent>& events,
    KernelEventType type) {
    std::uint32_t count = 0;
    for (const KernelEvent& event : events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

network_example::NetId spawned_projectile(const std::vector<KernelEvent>& events) {
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned) {
            return event.net_id;
        }
    }
    return 0;
}

network_example::NetId spawned_area_effect(const std::vector<KernelEvent>& events) {
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned &&
            event.code == static_cast<std::uint32_t>(network_example::EntityType::kAreaEffect)) {
            return event.net_id;
        }
    }
    return 0;
}

void deterministic_projectile_paths_match_motion_models() {
    const glm::vec3 origin{0.0f, 1.0f, 0.0f};
    const glm::vec3 velocity{10.0f, 5.0f, 0.0f};
    const glm::vec3 gravity{0.0f, -9.8f, 0.0f};

    const glm::vec3 rocket =
        network_example::projectile_position_at(
            origin,
            velocity,
            network_example::ProjectileMotionModel::kLinear,
            gravity,
            0.5f);
    assert(rocket.x > 4.99f && rocket.x < 5.01f);
    assert(rocket.y > 3.49f && rocket.y < 3.51f);

    const glm::vec3 grenade =
        network_example::projectile_position_at(
            origin,
            velocity,
            network_example::ProjectileMotionModel::kParabolic,
            gravity,
            0.5f);
    assert(grenade.x > 4.99f && grenade.x < 5.01f);
    assert(grenade.y > 2.26f && grenade.y < 2.28f);
    assert(grenade.y < rocket.y);
}

void rocket_moves_linearly_and_grenade_arcs() {
    network_example::World rocket_world;
    spawn_player(rocket_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<KernelEvent> rocket_events;
    PlayerInput rocket_input = fire_input(network_example::kWeaponSlot3);
    rocket_input.client_action_id = 9101;
    network_example::simulate_weapons(
        rocket_world,
        queue(rocket_input),
        0,
        &rocket_events);

    const network_example::NetId rocket = spawned_projectile(rocket_events);
    assert(rocket != 0);
    assert(projectile_state(rocket_world, rocket).motion_model ==
           network_example::ProjectileMotionModel::kLinear);
    network_example::simulate_projectiles(rocket_world, 0.1f, 1, &rocket_events);
    const network_example::Transform& rocket_transform =
        transform_state(rocket_world, rocket);
    assert(rocket_transform.position.x > 3.49f && rocket_transform.position.x < 3.51f);
    assert(rocket_transform.position.y > 0.99f && rocket_transform.position.y < 1.01f);

    network_example::World grenade_world;
    spawn_player(grenade_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<KernelEvent> grenade_events;
    PlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    grenade_input.client_action_id = 9102;
    network_example::simulate_weapons(
        grenade_world,
        queue(grenade_input),
        0,
        &grenade_events);

    const network_example::NetId grenade = spawned_projectile(grenade_events);
    assert(grenade != 0);
    assert(projectile_state(grenade_world, grenade).motion_model ==
           network_example::ProjectileMotionModel::kParabolic);
    network_example::simulate_projectiles(grenade_world, 0.1f, 1, &grenade_events);
    const network_example::Transform& grenade_transform =
        transform_state(grenade_world, grenade);
    assert(grenade_transform.position.x > 1.49f && grenade_transform.position.x < 1.51f);
    assert(grenade_transform.position.y > 0.94f && grenade_transform.position.y < 0.96f);
}

void projectile_damage_values_leave_enemy_flee_window() {
    network_example::World grenade_world;
    spawn_player(grenade_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<KernelEvent> grenade_events;
    network_example::simulate_weapons(
        grenade_world,
        queue(fire_input(network_example::kWeaponSlot2)),
        0,
        &grenade_events);

    const network_example::NetId grenade = spawned_projectile(grenade_events);
    require(grenade != 0);
    require(projectile_state(grenade_world, grenade).damage == 40);

    network_example::World rocket_world;
    spawn_player(rocket_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    std::vector<KernelEvent> rocket_events;
    network_example::simulate_weapons(
        rocket_world,
        queue(fire_input(network_example::kWeaponSlot3)),
        0,
        &rocket_events);

    const network_example::NetId rocket = spawned_projectile(rocket_events);
    require(rocket != 0);
    require(projectile_state(rocket_world, rocket).damage == 45);
}

void rejects_fire_during_cooldown_and_reload() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{5.0f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot0)),
        0,
        &events);
    assert(health(world, enemy).hp == 25);
    assert(weapon_state(world, player).ammo[network_example::kWeaponSlot0] == 29);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);

    events.clear();
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot0)),
        1,
        &events);
    assert(events.empty());
    assert(health(world, enemy).hp == 25);

    events.clear();
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot0)),
        3,
        &events);
    assert(health(world, enemy).hp == 0);
    assert(count_events(events, KernelEventType_DamageApplied) == 1);

    network_example::World reload_world;
    const network_example::NetId reload_player =
        spawn_player(reload_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId reload_enemy =
        spawn_enemy(reload_world, glm::vec3{5.0f, 0.0f, 0.0f});
    network_example::WeaponState& reload_weapon =
        weapon_state(reload_world, reload_player);
    reload_weapon.ammo[network_example::kWeaponSlot0] = 0;
    reload_weapon.reserve_ammo[network_example::kWeaponSlot0] = 30;

    PlayerInput reload_input{};
    reload_input.buttons = InputButton_Reload;
    reload_input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    events.clear();
    network_example::simulate_weapons(reload_world, queue(reload_input), 5, &events);
    assert(events.empty());
    assert(reload_weapon.is_reloading);

    events.clear();
    network_example::simulate_weapons(
        reload_world,
        queue(fire_input(network_example::kWeaponSlot0)),
        6,
        &events);
    assert(events.empty());
    assert(health(reload_world, reload_enemy).hp == 50);

    network_example::simulate_weapons(reload_world, {}, 35, &events);
    assert(!reload_weapon.is_reloading);
    assert(reload_weapon.ammo[network_example::kWeaponSlot0] == 30);

    events.clear();
    network_example::simulate_weapons(
        reload_world,
        queue(fire_input(network_example::kWeaponSlot0)),
        36,
        &events);
    assert(health(reload_world, reload_enemy).hp == 25);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);
}

void shotgun_applies_multiple_pellets() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{5.0f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot1)),
        0,
        &events);

    assert(health(world, enemy).hp == 0);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);
    assert(count_events(events, KernelEventType_DamageApplied) == 5);
}

void grenade_sweeps_and_explodes_with_falloff() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId near_enemy =
        spawn_enemy(world, glm::vec3{3.0f, 0.0f, 0.0f});
    const network_example::NetId far_enemy =
        spawn_enemy(world, glm::vec3{5.5f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    PlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    grenade_input.client_action_id = 4321;
    network_example::simulate_weapons(
        world,
        queue(grenade_input),
        0,
        &events);
    assert(count_events(events, KernelEventType_FireConfirmed) == 1);
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);

    network_example::NetId projectile = 0;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned) {
            projectile = event.net_id;
        }
    }
    assert(projectile != 0);
    const auto projectile_entity = world.find_entity(projectile);
    assert(projectile_entity.has_value());
    assert(
        world.registry()
            .get<network_example::NetworkIdentity>(*projectile_entity)
            .owner_peer == 1);
    assert(projectile_state(world, projectile).spawn_tick == 0);
    assert(projectile_state(world, projectile).client_action_id == 4321);

    events.clear();
    network_example::simulate_projectiles(world, 0.2f, 1, &events);

    assert(!world.find_entity(projectile).has_value());
    assert(health(world, near_enemy).hp > 0);
    assert(health(world, near_enemy).hp < 50);
    assert(health(world, far_enemy).hp > 0);
    assert(health(world, far_enemy).hp < 50);
    assert(health(world, near_enemy).hp < health(world, far_enemy).hp);
    assert(count_events(events, KernelEventType_Explosion) == 1);
    assert(count_events(events, KernelEventType_DamageApplied) >= 2);
}

void server_projectile_damage_to_player_is_pended() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId projectile_net_id =
        world.spawn_projectile(
            0,
            glm::vec3{0.0f, 1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::ProjectileState& projectile =
        projectile_state(world, projectile_net_id);
    projectile.weapon_id = network_example::kWeaponSlot2;
    projectile.damage = 80;
    projectile.explosion_radius = 4.0f;
    projectile.max_lifetime_seconds = 0.01f;

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.02f, 0, &events, &pipeline);
    assert(health(world, player).hp == 100);
    assert(count_events(events, KernelEventType_Explosion) == 1);
    assert(count_events(events, KernelEventType_DamageApplied) == 0);
    assert(pipeline.pending_count() == 1);

    pipeline.confirm_ready(world, 100000, 3, &events);
    assert(health(world, player).hp < 100);
    assert(count_events(events, KernelEventType_DamageApplied) == 1);
}

void direct_hit_projectile_without_explosion_applies_damage() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId projectile_net_id =
        world.spawn_projectile(
            1,
            glm::vec3{0.0f, 0.8f, 0.0f},
            glm::vec3{10.0f, 0.0f, 0.0f});
    network_example::ProjectileState& projectile =
        projectile_state(world, projectile_net_id);
    projectile.weapon_id = network_example::kWeaponSlot3;
    projectile.damage = 30;
    projectile.shooter_net_id = 1;
    projectile.explosion_radius = 0.0f;
    projectile.max_lifetime_seconds = 1.0f;
    projectile.initial_velocity = glm::vec3{10.0f, 0.0f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.1f, 1, &events);
    require(!world.find_entity(projectile_net_id).has_value());
    require(health(world, enemy).hp == 20);
    require(count_events(events, KernelEventType_Explosion) == 0);
    require(count_events(events, KernelEventType_DamageApplied) == 1);
}

void projectile_weapon_fires_again_after_cooldown() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    PlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    grenade_input.client_action_id = 8765;
    network_example::simulate_weapons(
        world,
        queue(grenade_input),
        0,
        &events);
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);

    events.clear();
    network_example::simulate_weapons(
        world,
        queue(grenade_input),
        1,
        &events);
    assert(events.empty());

    events.clear();
    network_example::simulate_weapons(
        world,
        queue(grenade_input),
        30,
        &events);
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);
    assert(weapon_state(world, player).ammo[network_example::kWeaponSlot2] == 28);
}

void projectile_rewind_spawns_from_historical_muzzle() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer history(8);
    history.write_frame(world, 4);
    world.registry().get<network_example::Transform>(*world.find_entity(player)).position =
        glm::vec3{10.0f, 0.0f, 0.0f};

    PlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    grenade_input.client_action_id = 9001;
    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(grenade_input),
        network_example::WeaponSimulationContext{
            &history,
            history.find_frame(4),
            nullptr,
            4,
            7,
            1.0f / 30.0f,
            133333},
        &events);

    const network_example::NetId projectile = spawned_projectile(events);
    assert(projectile != 0);
    const auto projectile_entity = world.find_entity(projectile);
    assert(projectile_entity.has_value());
    const network_example::Transform& transform =
        world.registry().get<network_example::Transform>(*projectile_entity);
    assert(transform.position.x > 1.49f);
    assert(transform.position.x < 1.51f);
    assert(transform.position.y > 0.95f);
    assert(transform.position.y < 0.96f);
    assert(projectile_state(world, projectile).spawn_tick == 4);
    assert(projectile_state(world, projectile).age_seconds > 0.09f);
}

void projectile_without_rewind_uses_current_muzzle() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{10.0f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot2)),
        7,
        &events);

    const network_example::NetId projectile = spawned_projectile(events);
    assert(projectile != 0);
    const auto projectile_entity = world.find_entity(projectile);
    assert(projectile_entity.has_value());
    const network_example::Transform& transform =
        world.registry().get<network_example::Transform>(*projectile_entity);
    assert(transform.position.x > 9.99f);
    assert(transform.position.x < 10.01f);
    assert(projectile_state(world, projectile).spawn_tick == 7);
    assert(projectile_state(world, projectile).age_seconds == 0.0f);
    (void)player;
}

void projectile_historical_hit_query_hits_historical_target() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{2.0f, 0.2f, 0.0f});
    network_example::HistoryBuffer history(12);
    for (std::uint32_t tick = 4; tick <= 8; ++tick) {
        history.write_frame(world, tick);
    }
    world.registry().get<network_example::Transform>(*world.find_entity(enemy)).position =
        glm::vec3{50.0f, 0.2f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot2)),
        network_example::WeaponSimulationContext{
            &history,
            history.find_frame(4),
            nullptr,
            4,
            8,
            1.0f / 30.0f,
            133333},
        &events);

    const network_example::NetId projectile = spawned_projectile(events);
    require(projectile != 0);
    require(!world.find_entity(projectile).has_value());
    require(health(world, enemy).hp < 50);
    require(count_events(events, KernelEventType_Explosion) == 1);
    require(count_events(events, KernelEventType_DamageApplied) >= 1);
}

void projectile_historical_hit_query_ignores_current_only_target() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{50.0f, 0.2f, 0.0f});
    network_example::HistoryBuffer history(12);
    for (std::uint32_t tick = 4; tick <= 8; ++tick) {
        history.write_frame(world, tick);
    }
    world.registry().get<network_example::Transform>(*world.find_entity(enemy)).position =
        glm::vec3{2.0f, 0.2f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        world,
        queue(fire_input(network_example::kWeaponSlot2)),
        network_example::WeaponSimulationContext{
            &history,
            history.find_frame(4),
            nullptr,
            4,
            8,
            1.0f / 30.0f,
            133333},
        &events);

    const network_example::NetId projectile = spawned_projectile(events);
    require(projectile != 0);
    const auto projectile_entity = world.find_entity(projectile);
    require(projectile_entity.has_value());
    const network_example::Transform& transform =
        world.registry().get<network_example::Transform>(*projectile_entity);
    require(transform.position.x > 1.99f);
    require(transform.position.x < 2.01f);
    require(health(world, enemy).hp == 50);
    require(count_events(events, KernelEventType_Explosion) == 0);
    require(count_events(events, KernelEventType_DamageApplied) == 0);
}

void rewind_hitscan_uses_historical_hit_volumes() {
    network_example::World current_world;
    spawn_player(current_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId current_enemy =
        spawn_enemy(current_world, glm::vec3{5.0f, 0.0f, 0.0f});
    const auto current_enemy_entity = current_world.find_entity(current_enemy);
    assert(current_enemy_entity.has_value());
    current_world.registry().get<network_example::Transform>(*current_enemy_entity).position =
        glm::vec3{120.0f, 0.0f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        current_world,
        queue(fire_input(network_example::kWeaponSlot0)),
        0,
        &events);
    assert(health(current_world, current_enemy).hp == 50);
    assert(count_events(events, KernelEventType_DamageApplied) == 0);

    network_example::World rewound_world;
    spawn_player(rewound_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId rewound_enemy =
        spawn_enemy(rewound_world, glm::vec3{5.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer history(4);
    history.write_frame(rewound_world, 4);
    const auto rewound_enemy_entity = rewound_world.find_entity(rewound_enemy);
    assert(rewound_enemy_entity.has_value());
    rewound_world.registry().get<network_example::Transform>(*rewound_enemy_entity).position =
        glm::vec3{20.0f, 0.0f, 0.0f};

    events.clear();
    network_example::simulate_weapons(
        rewound_world,
        queue(fire_input(network_example::kWeaponSlot0)),
        5,
        &events,
        history.find_frame(4));
    assert(health(rewound_world, rewound_enemy).hp == 25);
    assert(count_events(events, KernelEventType_DamageApplied) == 1);
}

void rewind_shotgun_respects_range() {
    network_example::World shotgun_world;
    spawn_player(shotgun_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(shotgun_world, glm::vec3{5.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer history(2);
    history.write_frame(shotgun_world, 2);
    shotgun_world.registry().get<network_example::Transform>(
        *shotgun_world.find_entity(enemy)).position = glm::vec3{50.0f, 0.0f, 0.0f};

    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        shotgun_world,
        queue(fire_input(network_example::kWeaponSlot1)),
        3,
        &events,
        history.find_frame(2));
    assert(health(shotgun_world, enemy).hp == 0);
    assert(count_events(events, KernelEventType_DamageApplied) == 5);

    network_example::World range_world;
    spawn_player(range_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId far_enemy =
        spawn_enemy(range_world, glm::vec3{120.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer range_history(1);
    range_history.write_frame(range_world, 1);
    events.clear();
    network_example::simulate_weapons(
        range_world,
        queue(fire_input(network_example::kWeaponSlot0)),
        2,
        &events,
        range_history.find_frame(1));
    assert(health(range_world, far_enemy).hp == 50);
    assert(count_events(events, KernelEventType_DamageApplied) == 0);
}

void area_effect_weapon_spawns_and_damages_enemy() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{1.4f, 0.0f, 0.0f});
    std::vector<KernelEvent> events;

    PlayerInput input = fire_input(network_example::kWeaponSlot4);
    network_example::simulate_weapons(world, queue(input), 4, &events);

    const network_example::NetId area = spawned_area_effect(events);
    require(area != 0);
    const auto area_entity = world.find_entity(area);
    require(area_entity.has_value());
    const network_example::AreaEffectState& state =
        world.registry().get<network_example::AreaEffectState>(*area_entity);
    require(state.radius == 2.0f);
    require(state.damage_per_interval == 12);
    require(state.damage_interval_ticks == 2);
    require(state.collision_mask == network_example::kCollisionLayerEnemy);

    network_example::DamagePipeline pipeline;
    network_example::simulate_area_effects(world, 4, 133333, &events, &pipeline);
    pipeline.confirm_ready(world, 133333, 4, &events);

    require(health(world, enemy).hp == 38);
    require(count_events(events, KernelEventType_DamageApplied) == 1);
}

void piercing_projectile_damages_sorted_targets_up_to_max_hit_count() {
    network_example::World world;
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId first =
        spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId second =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.0f});
    const network_example::NetId third =
        spawn_enemy(world, glm::vec3{3.0f, 0.0f, 0.0f});
    const network_example::NetId projectile =
        world.spawn_projectile(1, glm::vec3{0.0f, 0.8f, 0.0f}, glm::vec3{30.0f, 0.0f, 0.0f});
    network_example::ProjectileState& state = projectile_state(world, projectile);
    state.weapon_id = network_example::kWeaponSlot3;
    state.damage = 10;
    state.shooter_net_id = 1;
    state.hit_response = network_example::ProjectileHitResponse::kContinue;
    state.damage_shape = network_example::ProjectileDamageShape::kPiercingSegment;
    state.collision_mask = network_example::kCollisionLayerEnemy;
    state.max_hit_count = 2;
    state.max_lifetime_seconds = 1.0f;
    state.spawn_position = glm::vec3{0.0f, 0.8f, 0.0f};
    state.initial_velocity = glm::vec3{30.0f, 0.0f, 0.0f};
    std::vector<KernelEvent> events;

    network_example::simulate_projectiles(world, 0.1f, 5, &events);

    require(health(world, first).hp == 40);
    require(health(world, second).hp == 40);
    require(health(world, third).hp == 50);
    require(!world.find_entity(projectile).has_value());
    require(count_events(events, KernelEventType_DamageApplied) == 2);
}

void projectile_collision_mask_excludes_players() {
    network_example::World world;
    const network_example::NetId target_player =
        spawn_player(world, 2, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId projectile =
        world.spawn_projectile(1, glm::vec3{0.0f, 0.9f, 0.0f}, glm::vec3{20.0f, 0.0f, 0.0f});
    network_example::ProjectileState& state = projectile_state(world, projectile);
    state.weapon_id = network_example::kWeaponSlot3;
    state.damage = 10;
    state.shooter_net_id = 0;
    state.hit_response = network_example::ProjectileHitResponse::kDestroy;
    state.damage_shape = network_example::ProjectileDamageShape::kDirectHit;
    state.collision_mask = network_example::kCollisionLayerEnemy;
    state.max_lifetime_seconds = 1.0f;
    state.spawn_position = glm::vec3{0.0f, 0.9f, 0.0f};
    state.initial_velocity = glm::vec3{20.0f, 0.0f, 0.0f};
    std::vector<KernelEvent> events;

    network_example::simulate_projectiles(world, 0.1f, 6, &events);

    require(health(world, target_player).hp == 100);
    require(world.find_entity(projectile).has_value());
    require(count_events(events, KernelEventType_DamageApplied) == 0);
}

}  // namespace

int main() {
    deterministic_projectile_paths_match_motion_models();
    rocket_moves_linearly_and_grenade_arcs();
    projectile_damage_values_leave_enemy_flee_window();
    rejects_fire_during_cooldown_and_reload();
    shotgun_applies_multiple_pellets();
    grenade_sweeps_and_explodes_with_falloff();
    server_projectile_damage_to_player_is_pended();
    direct_hit_projectile_without_explosion_applies_damage();
    projectile_weapon_fires_again_after_cooldown();
    projectile_rewind_spawns_from_historical_muzzle();
    projectile_without_rewind_uses_current_muzzle();
    projectile_historical_hit_query_hits_historical_target();
    projectile_historical_hit_query_ignores_current_only_target();
    rewind_hitscan_uses_historical_hit_volumes();
    rewind_shotgun_respects_range();
    area_effect_weapon_spawns_and_damages_enemy();
    piercing_projectile_damages_sorted_targets_up_to_max_hit_count();
    projectile_collision_mask_excludes_players();
    return 0;
}
