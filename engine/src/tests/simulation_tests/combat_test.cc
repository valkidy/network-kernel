#include <cassert>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"

namespace {

void require_impl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at combat_test.cc:%d\n", line);
        std::abort();
    }
}

#define require(condition) require_impl((condition), __LINE__)

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

KernelPlayerInput fire_input(std::uint8_t weapon_id) {
    static std::uint32_t next_action_instance_id = 1u;
    KernelPlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.selected_weapon = weapon_id;
    input.action_intent = KernelActionIntent{
        next_action_instance_id,
        KernelActionBinding_PrimaryFire,
        0u,
        0u,
    };
    input.action_input = KernelActionInput{
        next_action_instance_id++, 1u, 0u, 0u};
    return input;
}

void set_action_instance(KernelPlayerInput& input, std::uint32_t action_instance_id) {
    input.action_intent.action_instance_id = action_instance_id;
    input.action_input.action_instance_id = action_instance_id;
}

KernelPlayerInput release_action(const KernelPlayerInput& active) {
    KernelPlayerInput release = active;
    release.action_intent = KernelActionIntent{};
    release.action_input.held = 0u;
    return release;
}

std::vector<network_example::QueuedInput> queue(KernelPlayerInput input) {
    return {network_example::QueuedInput{1, input}};
}

network_example::WeaponMechanicsDefinition weapon_definition(
    std::uint8_t id,
    network_example::WeaponFireMode mode,
    std::uint16_t magazine_size,
    std::uint16_t damage,
    std::uint32_t reload_ticks,
    float max_range = 0.0f) {
    network_example::WeaponMechanicsDefinition definition;
    definition.id = id;
    definition.mode = mode;
    definition.magazine_size = magazine_size;
    definition.damage = damage;
    (void)reload_ticks;
    definition.max_range = max_range;
    definition.pellet_count = 1;
    definition.fire_action_template_id = 1000u + id;
    definition.reload_action_template_id = 2000u + id;
    return definition;
}

void configure_projectile_response_templates(network_example::World& world) {
    network_example::RuntimeProjectileTemplate grenade_template;
    grenade_template.projectile_template_id = network_example::kWeaponSlot2;
    grenade_template.weapon_id = network_example::kWeaponSlot2;
    grenade_template.projectile_type = network_example::ProjectileType::kStandard;
    grenade_template.motion_model = network_example::ProjectileMotionModel::kParabolic;
    grenade_template.damage = 0;
    grenade_template.damage_shape =
        network_example::ProjectileDamageShape::kNone;
    grenade_template.speed = 15.0f;
    grenade_template.lifetime_ticks = 90;
    grenade_template.gravity = glm::vec3{0.0f, -9.8f, 0.0f};
    grenade_template.collision_mask = network_example::kCollisionLayerHostileSide;
    grenade_template.projectile_impact_binding =
        network_example::compile_spawn_projectile_binding(
            network_example::TriggerEventType::kProjectileImpact, 8);
    grenade_template.expired_binding =
        network_example::compile_spawn_projectile_binding(
            network_example::TriggerEventType::kExpired, 8);

    network_example::RuntimeProjectileTemplate area_template;
    area_template.projectile_template_id = 8;
    area_template.weapon_id = network_example::kWeaponSlot2;
    area_template.projectile_type = network_example::ProjectileType::kAreaEffect;
    area_template.damage = 40;
    area_template.area_radius = 4.0f;
    area_template.damage_interval_ticks = 45;
    area_template.lifetime_ticks = 45;
    area_template.collision_mask =
        network_example::kCollisionLayerPlayerSide | network_example::kCollisionLayerHostileSide;
    area_template.damage_falloff =
        network_example::ProjectileDamageFalloff::kLinear;

    network_example::RuntimeProjectileTemplate rocket_template;
    rocket_template.projectile_template_id = network_example::kWeaponSlot3;
    rocket_template.weapon_id = network_example::kWeaponSlot3;
    rocket_template.projectile_type = network_example::ProjectileType::kStandard;
    rocket_template.damage = 45;
    rocket_template.speed = 35.0f;
    rocket_template.lifetime_ticks = 75;
    rocket_template.collision_mask = network_example::kCollisionLayerHostileSide;

    network_example::RuntimeProjectileTemplate fire_floor_template;
    fire_floor_template.projectile_template_id = network_example::kWeaponId4;
    fire_floor_template.weapon_id = network_example::kWeaponId4;
    fire_floor_template.projectile_type =
        network_example::ProjectileType::kAreaEffect;
    fire_floor_template.damage = 12;
    fire_floor_template.area_radius = 2.0f;
    fire_floor_template.damage_interval_ticks = 2;
    fire_floor_template.lifetime_ticks = 6;
    fire_floor_template.collision_mask =
        network_example::kCollisionLayerHostileSide;

    world.set_projectile_templates({
        grenade_template,
        area_template,
        rocket_template,
        fire_floor_template,
    });
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
            30,
            100.0f),
        weapon_definition(
            network_example::kWeaponSlot1,
            network_example::WeaponFireMode::kShotgun,
            8,
            10,
            45,
            40.0f),
        weapon_definition(
            network_example::kWeaponSlot2,
            network_example::WeaponFireMode::kProjectile,
            30,
            40,
            60),
        weapon_definition(
            network_example::kWeaponSlot3,
            network_example::WeaponFireMode::kProjectile,
            6,
            45,
            75),
        weapon_definition(
            network_example::kWeaponId4,
            network_example::WeaponFireMode::kProjectile,
            3,
            12,
            30),
    }};
    tuning.definitions[network_example::kWeaponSlot1].pellet_count = 5;
    tuning.definitions[network_example::kWeaponSlot1].pellet_spread = 0.035f;
    tuning.definitions[network_example::kWeaponSlot2].projectile_template_id =
        network_example::kWeaponSlot2;
    tuning.definitions[network_example::kWeaponSlot3].projectile_template_id =
        network_example::kWeaponSlot3;
    tuning.definitions[network_example::kWeaponId4].projectile_template_id =
        network_example::kWeaponId4;
    configure_projectile_response_templates(world);
    world.set_action_templates({
        {1000u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 3u, 1u, 3u, 0u},
        {1001u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 20u, 1u, 20u, 0u},
        {1002u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 30u, 1u, 30u, 0u},
        {1003u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 45u, 1u, 45u, 0u},
        {1004u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 10u, 1u, 10u, 0u},
        {2000u, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
        {2001u, KernelActionTriggerMode_Press, 0u, 0u, 45u, 0u, 1u, 0u, 0u},
        {2002u, KernelActionTriggerMode_Press, 0u, 0u, 60u, 0u, 1u, 0u, 0u},
        {2003u, KernelActionTriggerMode_Press, 0u, 0u, 75u, 0u, 1u, 0u, 0u},
        {2004u, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
    });

    network_example::WeaponState& weapon =
        world.registry().get_or_emplace<network_example::WeaponState>(*entity);
    weapon.weapon_slot_count = network_example::kWeaponSlotCount;
    for (std::size_t slot = 0; slot < network_example::kWeaponSlotCount; ++slot) {
        weapon.weapon_ids[slot] = slot;
        weapon.ammo[slot] = tuning.definitions[slot].magazine_size;
        weapon.reserve_magazines[slot] = 3;
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
            event.code == static_cast<std::uint32_t>(network_example::EntityType::kProjectile)) {
            return event.net_id;
        }
    }
    return 0;
}

std::uint32_t projectile_count(const network_example::World& world) {
    return static_cast<std::uint32_t>(
        world.registry()
            .view<
                const network_example::ProjectileState,
                const network_example::ProjectileTag>()
            .size_hint());
}

void action_timeline_drives_rocket_rifle_and_beam() {
    const network_example::RuntimeActionTemplate rocket_action{
        1001,
        KernelActionTriggerMode_Press,
        KernelActionTemplateFlag_CancelOnDeath |
            KernelActionTemplateFlag_CancelOnWeaponChange |
            KernelActionTemplateFlag_CancelBeforeFirstCommit,
        1,
        2,
        30,
        1,
        2,
        0,
    };
    const network_example::RuntimeActionTemplate rifle_action{
        1002,
        KernelActionTriggerMode_Hold,
        KernelActionTemplateFlag_CancelOnRelease |
            KernelActionTemplateFlag_CancelOnDeath |
            KernelActionTemplateFlag_CancelOnWeaponChange,
        1,
        0,
        3,
        0,
        2,
        6,
    };
    const network_example::RuntimeActionTemplate beam_action{
        1003,
        KernelActionTriggerMode_Hold,
        KernelActionTemplateFlag_CancelOnRelease |
            KernelActionTemplateFlag_CancelOnDeath |
            KernelActionTemplateFlag_CancelOnWeaponChange |
            KernelActionTemplateFlag_CancelBeforeFirstCommit,
        1,
        5,
        1,
        0,
        2,
        6,
    };

    network_example::World rocket_world;
    const network_example::NetId rocket_player =
        spawn_player(rocket_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    rocket_world.set_action_templates({rocket_action, rifle_action, beam_action});
    const auto rocket_entity = rocket_world.find_entity(rocket_player);
    require(rocket_entity.has_value());
    network_example::WeaponTuning& rocket_tuning =
        rocket_world.registry().get<network_example::WeaponTuning>(*rocket_entity);
    rocket_tuning.definitions[network_example::kWeaponSlot3]
        .fire_action_template_id = rocket_action.action_template_id;
    std::vector<KernelEvent> events;
    KernelPlayerInput rocket_input = fire_input(network_example::kWeaponSlot3);
    set_action_instance(rocket_input, 7001u);
    network_example::simulate_weapons(rocket_world, queue(rocket_input), 0, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 0);
    network_example::simulate_weapons(rocket_world, {}, 1, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 0);
    network_example::simulate_weapons(rocket_world, {}, 2, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 1);
    require(
        rocket_world.registry().get<network_example::ActionRuntimeState>(*rocket_entity)
            .action_instance_id == 7001);
    events.clear();
    network_example::simulate_weapons(rocket_world, queue(rocket_input), 3, &events);
    require(events.empty());
    KernelPlayerInput release = release_action(rocket_input);
    network_example::simulate_weapons(rocket_world, queue(release), 4, &events);
    rocket_input.input_seq = 2;
    set_action_instance(rocket_input, 7002u);
    std::vector<network_example::ActionOutcome> cooldown_outcomes;
    network_example::simulate_weapons(
        rocket_world,
        queue(rocket_input),
        network_example::WeaponSimulationContext{
            nullptr, nullptr, nullptr, 5, 5, 0.0f, 0u, &cooldown_outcomes},
        &events);
    require(!cooldown_outcomes.empty());
    require(
        cooldown_outcomes.back().reason ==
        KernelLocalActionResultReason_Cooldown);
    network_example::simulate_weapons(rocket_world, {}, 7, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 0);
    rocket_input.input_seq = 3;
    set_action_instance(rocket_input, 7003u);
    network_example::simulate_weapons(rocket_world, queue(rocket_input), 30, &events);
    network_example::simulate_weapons(rocket_world, {}, 32, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 1);

    network_example::World rifle_world;
    const network_example::NetId rifle_player =
        spawn_player(rifle_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    rifle_world.set_action_templates({rocket_action, rifle_action, beam_action});
    const auto rifle_entity = rifle_world.find_entity(rifle_player);
    require(rifle_entity.has_value());
    network_example::WeaponTuning& rifle_tuning =
        rifle_world.registry().get<network_example::WeaponTuning>(*rifle_entity);
    rifle_tuning.definitions[network_example::kWeaponSlot0]
        .fire_action_template_id = rifle_action.action_template_id;
    KernelPlayerInput rifle_input = fire_input(network_example::kWeaponSlot0);
    set_action_instance(rifle_input, 7002u);
    events.clear();
    network_example::simulate_weapons(rifle_world, queue(rifle_input), 0, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 1);
    const network_example::ActionRuntimeState& rifle_runtime =
        rifle_world.registry().get<network_example::ActionRuntimeState>(*rifle_entity);
    require(rifle_runtime.action_instance_id != 0);
    network_example::simulate_weapons(rifle_world, {}, 1, &events);
    network_example::simulate_weapons(rifle_world, {}, 2, &events);
    network_example::simulate_weapons(rifle_world, {}, 3, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 2);
    release = release_action(rifle_input);
    network_example::simulate_weapons(rifle_world, queue(release), 4, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 2);

    network_example::World beam_world;
    const network_example::NetId beam_player =
        spawn_player(beam_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    beam_world.set_action_templates({rocket_action, rifle_action, beam_action});
    const auto beam_entity = beam_world.find_entity(beam_player);
    require(beam_entity.has_value());
    network_example::WeaponTuning& beam_tuning =
        beam_world.registry().get<network_example::WeaponTuning>(*beam_entity);
    beam_tuning.configured[network_example::kWeaponId5] = true;
    beam_tuning.definitions[network_example::kWeaponId5] = weapon_definition(
        network_example::kWeaponId5,
        network_example::WeaponFireMode::kProjectile,
        10,
        1,
        30);
    beam_tuning.definitions[network_example::kWeaponId5].projectile_template_id = 5;
    beam_tuning.definitions[network_example::kWeaponId5]
        .fire_action_template_id = beam_action.action_template_id;
    beam_tuning.definitions[network_example::kWeaponId5]
        .reload_action_template_id = 2004u;
    network_example::WeaponState& beam_weapon =
        beam_world.registry().get<network_example::WeaponState>(*beam_entity);
    beam_weapon.weapon_ids[0] = network_example::kWeaponId5;
    beam_weapon.ammo[0] = 10;
    network_example::RuntimeProjectileTemplate beam_template;
    beam_template.projectile_template_id = 5;
    beam_template.weapon_id = network_example::kWeaponId5;
    beam_template.projectile_type = network_example::ProjectileType::kBeam;
    beam_template.damage = 1;
    beam_template.lifetime_ticks = 30;
    beam_template.beam_length = 10.0f;
    beam_template.beam_radius = 0.25f;
    beam_world.set_projectile_templates({beam_template});
    KernelPlayerInput beam_input = fire_input(network_example::kWeaponId5);
    set_action_instance(beam_input, 8001u);
    events.clear();
    network_example::simulate_weapons(beam_world, queue(beam_input), 0, &events);
    release = release_action(beam_input);
    network_example::simulate_weapons(beam_world, queue(release), 2, &events);
    require(projectile_count(beam_world) == 0);
    beam_input.input_seq = 2;
    set_action_instance(beam_input, 8002u);
    network_example::simulate_weapons(beam_world, queue(beam_input), 4, &events);
    beam_input.action_intent = KernelActionIntent{};
    network_example::simulate_weapons(beam_world, queue(beam_input), 9, &events);
    require(projectile_count(beam_world) == 1);
    network_example::simulate_weapons(beam_world, queue(beam_input), 10, &events);
    require(projectile_count(beam_world) == 1);
    release = release_action(beam_input);
    release.input_seq = 3;
    network_example::simulate_weapons(beam_world, queue(release), 11, &events);
    require(projectile_count(beam_world) == 0);
}

void finite_press_and_per_weapon_gates_are_independent() {
    const network_example::RuntimeActionTemplate burst_action{
        1010,
        KernelActionTriggerMode_Press,
        KernelActionTemplateFlag_CancelOnDeath,
        1,
        0,
        2,
        3,
        2,
        0,
    };
    network_example::World burst_world;
    const network_example::NetId burst_player =
        spawn_player(burst_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    burst_world.set_action_templates({burst_action});
    const auto burst_entity = burst_world.find_entity(burst_player);
    require(burst_entity.has_value());
    network_example::WeaponTuning& burst_tuning =
        burst_world.registry().get<network_example::WeaponTuning>(*burst_entity);
    burst_tuning.definitions[network_example::kWeaponSlot0]
        .fire_action_template_id = burst_action.action_template_id;
    KernelPlayerInput burst_input = fire_input(network_example::kWeaponSlot0);
    set_action_instance(burst_input, 7100u);
    std::vector<KernelEvent> events;
    network_example::simulate_weapons(
        burst_world, queue(burst_input), 0, &events);
    network_example::simulate_weapons(burst_world, {}, 1, &events);
    network_example::simulate_weapons(burst_world, {}, 2, &events);
    network_example::simulate_weapons(burst_world, {}, 4, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 3);
    require(
        weapon_state(burst_world, burst_player)
            .ammo[network_example::kWeaponSlot0] == 27);
    require(
        burst_world.registry()
            .get<network_example::ActionRuntimeState>(*burst_entity)
            .phase == KernelActionPhase_Recovery);

    const network_example::RuntimeActionTemplate shared_action{
        1011,
        KernelActionTriggerMode_Press,
        KernelActionTemplateFlag_CancelOnDeath,
        1,
        0,
        3,
        1,
        0,
        0,
    };
    network_example::World switch_world;
    spawn_player(switch_world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    switch_world.set_action_templates({shared_action});
    const auto switch_entity = switch_world.find_entity(1);
    require(switch_entity.has_value());
    network_example::WeaponTuning& switch_tuning =
        switch_world.registry().get<network_example::WeaponTuning>(*switch_entity);
    switch_tuning.definitions[network_example::kWeaponSlot0]
        .fire_action_template_id = shared_action.action_template_id;
    switch_tuning.definitions[network_example::kWeaponSlot1]
        .fire_action_template_id = shared_action.action_template_id;

    KernelPlayerInput first = fire_input(network_example::kWeaponSlot0);
    set_action_instance(first, 7200u);
    events.clear();
    network_example::simulate_weapons(switch_world, queue(first), 0, &events);
    KernelPlayerInput other_weapon = fire_input(network_example::kWeaponSlot1);
    set_action_instance(other_weapon, 7201u);
    network_example::simulate_weapons(
        switch_world, queue(other_weapon), 1, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 2);

    KernelPlayerInput early_switch_back = fire_input(network_example::kWeaponSlot0);
    set_action_instance(early_switch_back, 7202u);
    std::vector<network_example::ActionOutcome> outcomes;
    network_example::simulate_weapons(
        switch_world,
        queue(early_switch_back),
        network_example::WeaponSimulationContext{
            nullptr, nullptr, nullptr, 1, 1, 0.0f, 0u, &outcomes},
        &events);
    require(!outcomes.empty());
    require(outcomes.back().reason == KernelLocalActionResultReason_Cooldown);

    KernelPlayerInput ready_switch_back = fire_input(network_example::kWeaponSlot0);
    set_action_instance(ready_switch_back, 7203u);
    network_example::simulate_weapons(
        switch_world, queue(ready_switch_back), 3, &events);
    require(count_events(events, KernelEventType_FireConfirmed) == 3);
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
    KernelPlayerInput rocket_input = fire_input(network_example::kWeaponSlot3);
    set_action_instance(rocket_input, 9101u);
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
    KernelPlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    set_action_instance(grenade_input, 9102u);
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
    require(projectile_state(grenade_world, grenade).damage == 0);

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
    reload_weapon.ammo[network_example::kWeaponSlot0] = 15;
    reload_weapon.reserve_magazines[network_example::kWeaponSlot0] = 1;

    KernelPlayerInput reload_input = fire_input(network_example::kWeaponSlot0);
    reload_input.action_intent.binding_id = KernelActionBinding_Reload;
    reload_input.action_input = KernelActionInput{};
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
    assert(reload_weapon.reserve_magazines[network_example::kWeaponSlot0] == 0);

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
    configure_projectile_response_templates(world);
    spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId near_enemy =
        spawn_enemy(world, glm::vec3{3.0f, 0.0f, 0.0f});
    const network_example::NetId far_enemy =
        spawn_enemy(world, glm::vec3{5.5f, 0.0f, 0.0f});

    std::vector<KernelEvent> events;
    KernelPlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    set_action_instance(grenade_input, 4321u);
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
    assert(projectile_state(world, projectile).action_instance_id == 4321);

    events.clear();
    network_example::simulate_projectiles(world, 0.2f, 1, &events);

    assert(!world.find_entity(projectile).has_value());
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);
    network_example::simulate_area_effects(world, 1, &events, nullptr);
    assert(health(world, near_enemy).hp > 0);
    assert(health(world, near_enemy).hp < 50);
    assert(health(world, far_enemy).hp > 0);
    assert(health(world, far_enemy).hp < 50);
    assert(health(world, near_enemy).hp < health(world, far_enemy).hp);
    assert(count_events(events, KernelEventType_DamageApplied) >= 2);
}

void server_projectile_damage_to_player_is_pended() {
    network_example::World world;
    configure_projectile_response_templates(world);
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
    projectile.projectile_template_id = network_example::kWeaponSlot2;
    projectile.damage = 80;
    projectile.max_lifetime_ticks = 1;
    const auto projectile_entity = world.find_entity(projectile_net_id);
    assert(projectile_entity.has_value());
    world.registry().emplace<network_example::OnExpiredTriggerTag>(
        *projectile_entity);

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.02f, 0, &events, &pipeline);
    assert(health(world, player).hp == 100);
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);
    network_example::simulate_area_effects(world, 0, 0, &events, &pipeline);
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
    projectile.max_lifetime_ticks = 30;
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
    KernelPlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    set_action_instance(grenade_input, 8765u);
    network_example::simulate_weapons(
        world,
        queue(grenade_input),
        0,
        &events);
    assert(count_events(events, KernelEventType_EntitySpawned) == 1);

    events.clear();
    set_action_instance(grenade_input, 8766u);
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

void local_predicted_spammer_can_spawn_many_low_damage_projectiles() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto player_entity = world.find_entity(player);
    assert(player_entity.has_value());
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*player_entity);
    network_example::WeaponMechanicsDefinition& spammer =
        tuning.definitions[network_example::kWeaponSlot2];
    spammer.mode = network_example::WeaponFireMode::kProjectile;
    spammer.magazine_size = 120;
    spammer.damage = 1;
    spammer.fire_action_template_id = 1002u;
    spammer.reload_action_template_id = 2002u;
    spammer.projectile_template_id = network_example::kWeaponSlot2;
    network_example::RuntimeProjectileTemplate spammer_template;
    spammer_template.projectile_template_id = network_example::kWeaponSlot2;
    spammer_template.weapon_id = network_example::kWeaponSlot2;
    spammer_template.projectile_type = network_example::ProjectileType::kStandard;
    spammer_template.damage = 1;
    spammer_template.speed = 30.0f;
    spammer_template.lifetime_ticks = 60;
    spammer_template.collision_mask = network_example::kCollisionLayerHostileSide;
    world.set_projectile_templates({spammer_template});
    world.set_action_templates({
        {1002u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 1u, 1u, 0u, 0u},
        {2002u, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
    });
    network_example::WeaponState& weapon = weapon_state(world, player);
    weapon.ammo[network_example::kWeaponSlot2] = spammer.magazine_size;

    std::vector<KernelEvent> all_events;
    for (std::uint32_t tick = 0; tick < 12; ++tick) {
        KernelPlayerInput input = fire_input(network_example::kWeaponSlot2);
        input.input_seq = tick + 1;
        set_action_instance(input, 10000u + tick);
        std::vector<KernelEvent> events;
        network_example::simulate_weapons(
            world,
            queue(input),
            tick,
            &events);
        all_events.insert(all_events.end(), events.begin(), events.end());
    }

    require(count_events(all_events, KernelEventType_EntitySpawned) == 12);
    require(projectile_count(world) == 12);
    require(weapon.ammo[network_example::kWeaponSlot2] == 108);
    auto view =
        world.registry()
            .view<
                const network_example::ProjectileState,
                const network_example::ProjectileTag>();
    for (const entt::entity entity : view) {
        const network_example::ProjectileState& projectile =
            view.get<const network_example::ProjectileState>(entity);
        require(projectile.damage == 1);
        require(projectile.weapon_id == network_example::kWeaponSlot2);
        require(
            projectile.motion_model ==
            network_example::ProjectileMotionModel::kLinear);
    }
}

void projectile_spammer_burst_spawns_three_spread_projectiles() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto player_entity = world.find_entity(player);
    assert(player_entity.has_value());
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*player_entity);
    network_example::WeaponMechanicsDefinition& spammer =
        tuning.definitions[network_example::kWeaponSlot2];
    spammer.mode = network_example::WeaponFireMode::kProjectile;
    spammer.magazine_size = 120;
    spammer.damage = 1;
    spammer.fire_action_template_id = 1002u;
    spammer.reload_action_template_id = 2002u;
    spammer.pellet_count = 3;
    spammer.pellet_spread = 15.0f;
    spammer.projectile_template_id = network_example::kWeaponSlot2;
    network_example::RuntimeProjectileTemplate spammer_template;
    spammer_template.projectile_template_id = network_example::kWeaponSlot2;
    spammer_template.weapon_id = network_example::kWeaponSlot2;
    spammer_template.projectile_type = network_example::ProjectileType::kStandard;
    spammer_template.damage = 1;
    spammer_template.speed = 30.0f;
    spammer_template.lifetime_ticks = 60;
    spammer_template.collision_mask = network_example::kCollisionLayerHostileSide;
    world.set_projectile_templates({spammer_template});
    world.set_action_templates({
        {1002u, KernelActionTriggerMode_Press, 0u, 1u, 0u, 1u, 1u, 0u, 0u},
        {2002u, KernelActionTriggerMode_Press, 0u, 0u, 30u, 0u, 1u, 0u, 0u},
    });
    network_example::WeaponState& weapon = weapon_state(world, player);
    weapon.ammo[network_example::kWeaponSlot2] = spammer.magazine_size;

    KernelPlayerInput input = fire_input(network_example::kWeaponSlot2);
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    std::vector<KernelEvent> events;
    network_example::simulate_weapons(world, queue(input), 0, &events);

    require(count_events(events, KernelEventType_EntitySpawned) == 3);
    require(projectile_count(world) == 3);
    require(weapon.ammo[network_example::kWeaponSlot2] == 119);

    bool saw_negative = false;
    bool saw_center = false;
    bool saw_positive = false;
    auto view =
        world.registry()
            .view<
                const network_example::ProjectileState,
                const network_example::ProjectileTag>();
    for (const entt::entity entity : view) {
        const network_example::ProjectileState& projectile =
            view.get<const network_example::ProjectileState>(entity);
        require(projectile.damage == 1);
        require(projectile.weapon_id == network_example::kWeaponSlot2);
        const glm::vec3 direction = glm::normalize(projectile.initial_velocity);
        saw_center = saw_center || (std::fabs(direction.z) < 0.001f);
        saw_negative = saw_negative || direction.z < -0.25f;
        saw_positive = saw_positive || direction.z > 0.25f;
    }
    require(saw_negative);
    require(saw_center);
    require(saw_positive);
}

void projectile_rewind_spawns_from_historical_muzzle() {
    network_example::World world;
    const network_example::NetId player =
        spawn_player(world, 1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::HistoryBuffer history(8);
    history.write_frame(world, 4);
    world.registry().get<network_example::Transform>(*world.find_entity(player)).position =
        glm::vec3{10.0f, 0.0f, 0.0f};

    KernelPlayerInput grenade_input = fire_input(network_example::kWeaponSlot2);
    set_action_instance(grenade_input, 9001u);
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
    assert(projectile_state(world, projectile).age_ticks == 3u);
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
    assert(projectile_state(world, projectile).age_ticks == 0u);
    assert(player != 0);
}

void projectile_historical_hit_emits_impact_trigger() {
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
    require(count_events(events, KernelEventType_EntitySpawned) == 2);
    require(health(world, enemy).hp == 50);
    require(count_events(events, KernelEventType_Explosion) == 0);
    require(count_events(events, KernelEventType_DamageApplied) == 0);
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

    network_example::WeaponState& weapon = weapon_state(world, 1);
    weapon.weapon_ids[0] = network_example::kWeaponId4;
    weapon.ammo[0] = 3;
    KernelPlayerInput input = fire_input(network_example::kWeaponId4);
    network_example::simulate_weapons(world, queue(input), 4, &events);

    const network_example::NetId area = spawned_area_effect(events);
    require(area != 0);
    const auto area_entity = world.find_entity(area);
    require(area_entity.has_value());
    const network_example::ProjectileAreaEffectRuntime& state =
        world.registry().get<network_example::ProjectileAreaEffectRuntime>(
            *area_entity);
    require(state.radius == 2.0f);
    require(state.damage_per_interval == 12);
    require(state.damage_interval_ticks == 2);
    require(state.collision_mask == network_example::kCollisionLayerHostileSide);

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
    state.collision_mask = network_example::kCollisionLayerHostileSide;
    state.max_hit_count = 2;
    state.max_lifetime_ticks = 30;
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

void sphere_projectile_uses_swept_collider_geometry() {
    network_example::World world;
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.7f});
    const network_example::NetId projectile =
        world.spawn_projectile(
            1,
            glm::vec3{0.0f, 0.8f, 0.0f},
            glm::vec3{20.0f, 0.0f, 0.0f});
    network_example::ProjectileState& state = projectile_state(world, projectile);
    state.weapon_id = network_example::kWeaponSlot3;
    state.damage = 10;
    state.hit_response = network_example::ProjectileHitResponse::kDestroy;
    state.damage_shape = network_example::ProjectileDamageShape::kDirectHit;
    state.collision_mask = network_example::kCollisionLayerHostileSide;
    state.max_lifetime_ticks = 30;
    state.spawn_position = glm::vec3{0.0f, 0.8f, 0.0f};
    state.initial_velocity = glm::vec3{20.0f, 0.0f, 0.0f};
    state.has_collision_geometry = true;
    state.collision_geometry.shape_type = network_example::ColliderShapeType::kSphere;
    state.collision_geometry.radius = 0.3f;
    state.collision_query_mode =
        network_example::ProjectileCollisionQueryMode::kAuto;
    std::vector<KernelEvent> events;

    network_example::simulate_projectiles(world, 0.1f, 7, &events);

    require(!world.find_entity(projectile).has_value());
    require(health(world, enemy).hp == 40);
    require(count_events(events, KernelEventType_DamageApplied) == 1);
}

void box_projectile_uses_swept_collider_geometry() {
    network_example::World world;
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.75f});
    const network_example::NetId projectile =
        world.spawn_projectile(
            1,
            glm::vec3{0.0f, 0.8f, 0.0f},
            glm::vec3{20.0f, 0.0f, 0.0f});
    network_example::ProjectileState& state = projectile_state(world, projectile);
    state.weapon_id = network_example::kWeaponSlot3;
    state.damage = 10;
    state.hit_response = network_example::ProjectileHitResponse::kDestroy;
    state.damage_shape = network_example::ProjectileDamageShape::kDirectHit;
    state.collision_mask = network_example::kCollisionLayerHostileSide;
    state.max_lifetime_ticks = 30;
    state.spawn_position = glm::vec3{0.0f, 0.8f, 0.0f};
    state.initial_velocity = glm::vec3{20.0f, 0.0f, 0.0f};
    state.has_collision_geometry = true;
    state.collision_geometry.shape_type = network_example::ColliderShapeType::kAabb;
    state.collision_geometry.half_extents = glm::vec3{0.2f, 0.2f, 0.35f};
    state.collision_query_mode =
        network_example::ProjectileCollisionQueryMode::kAuto;
    std::vector<KernelEvent> events;

    network_example::simulate_projectiles(world, 0.1f, 8, &events);

    require(!world.find_entity(projectile).has_value());
    require(health(world, enemy).hp == 40);
    require(count_events(events, KernelEventType_DamageApplied) == 1);
}

void overlap_query_mode_does_not_sweep_moving_projectile() {
    network_example::World world;
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.7f});
    const network_example::NetId projectile =
        world.spawn_projectile(
            1,
            glm::vec3{0.0f, 0.8f, 0.0f},
            glm::vec3{20.0f, 0.0f, 0.0f});
    network_example::ProjectileState& state = projectile_state(world, projectile);
    state.weapon_id = network_example::kWeaponSlot3;
    state.damage = 10;
    state.hit_response = network_example::ProjectileHitResponse::kDestroy;
    state.damage_shape = network_example::ProjectileDamageShape::kDirectHit;
    state.collision_mask = network_example::kCollisionLayerHostileSide;
    state.max_lifetime_ticks = 30;
    state.spawn_position = glm::vec3{0.0f, 0.8f, 0.0f};
    state.initial_velocity = glm::vec3{20.0f, 0.0f, 0.0f};
    state.has_collision_geometry = true;
    state.collision_geometry.shape_type = network_example::ColliderShapeType::kSphere;
    state.collision_geometry.radius = 0.3f;
    state.collision_query_mode =
        network_example::ProjectileCollisionQueryMode::kOverlap;
    std::vector<KernelEvent> events;

    network_example::simulate_projectiles(world, 0.1f, 9, &events);

    require(world.find_entity(projectile).has_value());
    require(health(world, enemy).hp == 50);
    require(count_events(events, KernelEventType_DamageApplied) == 0);
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
    state.collision_mask = network_example::kCollisionLayerHostileSide;
    state.max_lifetime_ticks = 30;
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
    action_timeline_drives_rocket_rifle_and_beam();
    finite_press_and_per_weapon_gates_are_independent();
    deterministic_projectile_paths_match_motion_models();
    rocket_moves_linearly_and_grenade_arcs();
    projectile_damage_values_leave_enemy_flee_window();
    rejects_fire_during_cooldown_and_reload();
    shotgun_applies_multiple_pellets();
    grenade_sweeps_and_explodes_with_falloff();
    server_projectile_damage_to_player_is_pended();
    direct_hit_projectile_without_explosion_applies_damage();
    projectile_weapon_fires_again_after_cooldown();
    local_predicted_spammer_can_spawn_many_low_damage_projectiles();
    projectile_spammer_burst_spawns_three_spread_projectiles();
    projectile_rewind_spawns_from_historical_muzzle();
    projectile_without_rewind_uses_current_muzzle();
    projectile_historical_hit_emits_impact_trigger();
    projectile_historical_hit_query_ignores_current_only_target();
    rewind_hitscan_uses_historical_hit_volumes();
    rewind_shotgun_respects_range();
    area_effect_weapon_spawns_and_damages_enemy();
    piercing_projectile_damages_sorted_targets_up_to_max_hit_count();
    sphere_projectile_uses_swept_collider_geometry();
    box_projectile_uses_swept_collider_geometry();
    overlap_query_mode_does_not_sweep_moving_projectile();
    projectile_collision_mask_excludes_players();
    return 0;
}
