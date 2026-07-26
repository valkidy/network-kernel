#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/simulation.h"
#include "world/public/world.h"

namespace {

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
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
    projectile.max_lifetime_ticks = 30;
    return net_id;
}

network_example::RuntimeProjectileTemplate area_effect_template(
    std::uint32_t template_id,
    std::uint8_t weapon_id,
    float radius,
    std::uint16_t damage,
    std::uint32_t damage_interval_ticks,
    std::uint32_t lifetime_ticks,
    std::uint32_t collision_mask) {
    network_example::RuntimeProjectileTemplate projectile_template;
    projectile_template.projectile_template_id = template_id;
    projectile_template.weapon_id = weapon_id;
    projectile_template.projectile_type = network_example::ProjectileType::kAreaEffect;
    projectile_template.motion_model = network_example::ProjectileMotionModel::kLinear;
    projectile_template.damage_shape = network_example::ProjectileDamageShape::kDirectHit;
    projectile_template.damage = damage;
    projectile_template.damage_interval_ticks = damage_interval_ticks;
    projectile_template.lifetime_ticks = lifetime_ticks;
    projectile_template.area_radius = radius;
    projectile_template.collision_mask = collision_mask;
    return projectile_template;
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
    world.set_projectile_templates({
        area_effect_template(
            8,
            9,
            2.5f,
            12,
            3,
            7,
            network_example::kCollisionLayerHostileSide),
    });

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 3;
    rule.rhs_weapon_id = 4;
    rule.symmetric = true;
    rule.destroy_lhs = true;
    rule.destroy_rhs = true;
    rule.spawn_projectile_template_id = 8;
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
                    static_cast<std::uint32_t>(network_example::EntityType::kProjectile));
        }
    }
    const auto area_entity = world.find_entity(area_effect_net_id);
    require(area_entity.has_value());
    require(world.registry().all_of<network_example::ProjectileAreaEffectRuntime>(
        *area_entity));
    const network_example::ProjectileAreaEffectRuntime& area_effect =
        world.registry().get<network_example::ProjectileAreaEffectRuntime>(
            *area_entity);
    require(area_effect.radius == 2.5f);
    require(area_effect.damage_interval_ticks == 3);
    require(area_effect.expire_tick == 12);
    require(area_effect.damage_per_interval == 12);
    require(area_effect.source_code == 9);
    require(area_effect.collision_mask == network_example::kCollisionLayerHostileSide);
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

void interaction_uses_swept_projectile_collision_geometry() {
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
        glm::vec3{1.0f, 0.5f, 0.35f},
        glm::vec3{0.0f, 0.0f, 0.0f},
        2);
    const auto lhs_entity = world.find_entity(lhs);
    require(lhs_entity.has_value());
    network_example::ProjectileState& lhs_projectile =
        world.registry().get<network_example::ProjectileState>(*lhs_entity);
    lhs_projectile.has_collision_geometry = true;
    lhs_projectile.collision_geometry.shape_type =
        network_example::ColliderShapeType::kSphere;
    lhs_projectile.collision_geometry.radius = 0.25f;
    lhs_projectile.collision_query_mode =
        network_example::ProjectileCollisionQueryMode::kAuto;

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 1;
    rule.rhs_weapon_id = 2;
    rule.symmetric = true;
    rule.destroy_lhs = true;
    rule.destroy_rhs = true;
    world.add_projectile_interaction_rule(rule);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.1f, 1, &events);

    require(!world.find_entity(lhs).has_value());
    require(!world.find_entity(rhs).has_value());
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
    world.set_projectile_templates({
        area_effect_template(
            8,
            9,
            1.0f,
            0,
            1,
            3,
            network_example::kCollisionMaskDamageable),
    });

    network_example::ProjectileInteractionRule rule;
    rule.lhs_weapon_id = 1;
    rule.rhs_weapon_id = 2;
    rule.symmetric = true;
    rule.destroy_lhs = true;
    rule.destroy_rhs = true;
    rule.spawn_projectile_template_id = 8;
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

void action_graph_binding_validation_is_typed_and_authoritative() {
    network_example::CompiledActionGraphBinding binding =
        network_example::compile_spawn_projectile_binding(
            network_example::TriggerEventType::kProjectileImpact, 8);
    std::string error;
    require(network_example::validate_action_graph_binding(binding, &error));

    network_example::CompiledActionGraphBinding undeclared = binding;
    undeclared.parameters.push_back(network_example::ActionGraphParameterBinding{
        "unknown",
        network_example::ActionGraphParameterValue{1.0f},
    });
    require(!network_example::validate_action_graph_binding(undeclared, &error));

    network_example::CompiledActionGraphBinding wrong_type = binding;
    wrong_type.parameters[0].expression =
        network_example::ActionGraphParameterValue{
            network_example::EntityIdValue{8}};
    require(!network_example::validate_action_graph_binding(wrong_type, &error));

    const network_example::TriggerEvent event{
        network_example::TriggerEventType::kProjectileImpact,
        10,
        20,
        30,
        glm::vec3{1.0f, 2.0f, 3.0f},
        glm::vec3{0.0f, 1.0f, 0.0f},
        network_example::ProjectileImpactPayload{3, 40, 5, false},
    };
    network_example::ActionExecutionProvenance provenance;
    provenance.request_id = 50;
    provenance.action_instance_id = 40;
    provenance.server_tick = 6;
    provenance.instigator = 20;
    provenance.owner_peer = 7;
    provenance.source_weapon_id = 5;
    provenance.authority_source =
        network_example::ActionAuthoritySource::kClientPrediction;
    std::vector<network_example::SpawnProjectileCommand> commands;
    require(network_example::evaluate_action_graph(
        binding, 10, event, provenance, &commands, &error));
    require(commands.empty());

    provenance.authority_source =
        network_example::ActionAuthoritySource::kAuthoritativeSimulation;
    require(network_example::evaluate_action_graph(
        binding, 10, event, provenance, &commands, &error));
    require(commands.size() == 1);
    require(commands[0].projectile_template_id == 8);
    require(commands[0].position == event.position);
    require(commands[0].direction == event.direction);
    require(commands[0].provenance.action_instance_id == 40);
    require(commands[0].provenance.instigator == 20);
    require(commands[0].provenance.owner_peer == 7);
    require(commands[0].provenance.source_weapon_id == 5);
}

void impact_response_spawns_area_effect_projectile_once() {
    network_example::World world;
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{1.0f, 0.2f, 0.0f});
    const auto enemy_entity = world.find_entity(enemy);
    require(enemy_entity.has_value());
    network_example::Health& health =
        world.registry().get<network_example::Health>(*enemy_entity);
    health.hp = 100;
    health.max_hp = 100;

    network_example::RuntimeProjectileTemplate rocket_template;
    rocket_template.projectile_template_id = 3;
    rocket_template.weapon_id = 3;
    rocket_template.projectile_type = network_example::ProjectileType::kStandard;
    rocket_template.impact_spawn_projectile_template_id = 8;
    rocket_template.impact_destroy_self = 1u;

    world.set_projectile_templates({
        rocket_template,
        area_effect_template(
            8,
            3,
            2.5f,
            45,
            45,
            45,
            network_example::kCollisionLayerHostileSide),
    });

    const network_example::NetId rocket = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.2f, 0.0f},
        glm::vec3{20.0f, 0.0f, 0.0f},
        3,
        network_example::kCollisionLayerHostileSide);
    const auto rocket_entity = world.find_entity(rocket);
    require(rocket_entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*rocket_entity);
    projectile.projectile_template_id = 3;
    projectile.shooter_net_id = rocket;
    projectile.damage = 0;
    projectile.damage_shape = network_example::ProjectileDamageShape::kNone;
    world.registry().emplace<network_example::OnProjectileImpactTriggerTag>(
        *rocket_entity);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 1, &events);

    require(!world.find_entity(rocket).has_value());
    std::vector<network_example::NetId> area_effects;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned &&
            event.code ==
                static_cast<std::uint32_t>(network_example::EntityType::kProjectile)) {
            area_effects.push_back(event.net_id);
        }
    }
    require(area_effects.size() == 1);

    const auto area_entity = world.find_entity(area_effects[0]);
    require(area_entity.has_value());
    const network_example::ProjectileAreaEffectRuntime& area_effect =
        world.registry().get<network_example::ProjectileAreaEffectRuntime>(
            *area_entity);
    require(area_effect.damage_per_interval == 45);
    require(area_effect.damage_interval_ticks == 45);
    require(area_effect.expire_tick == 46);

    network_example::simulate_area_effects(world, 1, &events, nullptr);
    require(health.hp == 55);
    network_example::simulate_area_effects(world, 2, &events, nullptr);
    require(health.hp == 55);
}

void impact_response_is_additive_with_direct_hit_damage() {
    network_example::World world;
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{1.0f, 0.5f, 0.0f});
    const auto enemy_entity = world.find_entity(enemy);
    require(enemy_entity.has_value());
    network_example::Health& health =
        world.registry().get<network_example::Health>(*enemy_entity);
    health.hp = 100;
    health.max_hp = 100;

    network_example::RuntimeProjectileTemplate rocket_template;
    rocket_template.projectile_template_id = 3;
    rocket_template.impact_spawn_projectile_template_id = 8;
    rocket_template.impact_destroy_self = true;
    world.set_projectile_templates({
        rocket_template,
        area_effect_template(
            8,
            3,
            2.5f,
            45,
            45,
            45,
            network_example::kCollisionLayerHostileSide),
    });

    const network_example::NetId rocket = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{20.0f, 0.0f, 0.0f},
        3,
        network_example::kCollisionLayerHostileSide);
    const auto rocket_entity = world.find_entity(rocket);
    require(rocket_entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*rocket_entity);
    projectile.projectile_template_id = 3;
    projectile.shooter_net_id = rocket;
    projectile.damage = 45;
    projectile.damage_shape =
        network_example::ProjectileDamageShape::kDirectHit;
    world.registry().emplace<network_example::OnProjectileImpactTriggerTag>(
        *rocket_entity);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 1, &events);
    require(health.hp == 55);

    network_example::simulate_area_effects(world, 1, &events, nullptr);
    require(health.hp == 10);
}

void world_impact_emits_impact_response_once() {
    network_example::World world;
    network_example::physics::PhysicsWorld physics;
    world.set_collision_world(&physics);

    network_example::physics::CollisionObjectDescriptor obstacle;
    obstacle.identity = network_example::physics::CollisionObjectIdentity{
        0,
        100,
        0,
        network_example::physics::CollisionObjectKind::kStaticObstacle,
        network_example::physics::CollisionLayer::kStaticObstacle,
    };
    obstacle.shape.type = network_example::physics::CollisionShapeType::kBox;
    obstacle.shape.half_extents = glm::vec3{0.1f, 1.0f, 1.0f};
    obstacle.position = glm::vec3{1.0f, 0.5f, 0.0f};
    std::string error;
    require(physics.upsert_object(obstacle, &error));

    network_example::RuntimeProjectileTemplate rocket_template;
    rocket_template.projectile_template_id = 3;
    rocket_template.impact_spawn_projectile_template_id = 8;
    world.set_projectile_templates({
        rocket_template,
        area_effect_template(
            8,
            3,
            2.5f,
            45,
            45,
            45,
            network_example::kCollisionLayerHostileSide),
    });

    const network_example::NetId rocket = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{20.0f, 0.0f, 0.0f},
        3);
    const auto rocket_entity = world.find_entity(rocket);
    require(rocket_entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*rocket_entity);
    projectile.projectile_template_id = 3;
    projectile.shooter_net_id = rocket;
    world.registry().emplace<network_example::OnProjectileImpactTriggerTag>(
        *rocket_entity);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.1f, 1, &events);

    require(!world.find_entity(rocket).has_value());
    require(count_events(events, KernelEventType_EntitySpawned) == 1);
    require(count_events(events, KernelEventType_DamageApplied) == 0);
}

void historical_hit_emits_impact_response_once() {
    network_example::World world;
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{1.0f, 0.5f, 0.0f});
    const auto enemy_entity = world.find_entity(enemy);
    require(enemy_entity.has_value());
    world.registry().get<network_example::Health>(*enemy_entity).hp = 100;
    network_example::HistoryBuffer history(4);
    history.write_frame(world, 1);

    network_example::RuntimeProjectileTemplate rocket_template;
    rocket_template.projectile_template_id = 3;
    rocket_template.impact_spawn_projectile_template_id = 8;
    world.set_projectile_templates({
        rocket_template,
        area_effect_template(
            8,
            3,
            2.5f,
            45,
            45,
            45,
            network_example::kCollisionLayerHostileSide),
    });

    const network_example::NetId rocket = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{20.0f, 0.0f, 0.0f},
        3);
    const auto rocket_entity = world.find_entity(rocket);
    require(rocket_entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*rocket_entity);
    projectile.projectile_template_id = 3;
    projectile.shooter_net_id = rocket;
    world.registry().emplace<network_example::OnProjectileImpactTriggerTag>(
        *rocket_entity);

    const network_example::HistoryFrame* frame = history.find_frame(1);
    require(frame != nullptr);
    require(frame->volumes.size() == 1);
    require(frame->volumes[0].net_id == enemy);
    require(frame->volumes[0].alive != 0);
    network_example::HistoricalHitResult historical_hit;
    require(network_example::sweep_history_frame(
        *frame,
        glm::vec3{0.0f, 0.2f, 0.0f},
        glm::vec3{2.0f, 0.2f, 0.0f},
        rocket,
        &historical_hit));

    std::vector<KernelEvent> events;
    require(network_example::resolve_projectile_historical_hit(
        world,
        history,
        rocket,
        rocket,
        1,
        projectile,
        glm::vec3{0.0f, 0.2f, 0.0f},
        glm::vec3{20.0f, 0.0f, 0.0f},
        0,
        1,
        0.1f,
        &events,
        nullptr));

    require(enemy != 0);
    require(!world.find_entity(rocket).has_value());
    require(count_events(events, KernelEventType_EntitySpawned) == 1);
}

void expired_response_does_not_reuse_impact_response() {
    network_example::World world;
    network_example::RuntimeProjectileTemplate projectile_template;
    projectile_template.projectile_template_id = 3;
    projectile_template.impact_spawn_projectile_template_id = 8;
    projectile_template.expire_spawn_projectile_template_id = 9;
    world.set_projectile_templates({
        projectile_template,
        area_effect_template(
            8,
            8,
            1.0f,
            1,
            1,
            2,
            network_example::kCollisionMaskDamageable),
        area_effect_template(
            9,
            9,
            1.0f,
            1,
            1,
            2,
            network_example::kCollisionMaskDamageable),
    });

    const network_example::NetId source = spawn_test_projectile(
        world,
        1,
        glm::vec3{0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        3);
    const auto source_entity = world.find_entity(source);
    require(source_entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*source_entity);
    projectile.projectile_template_id = 3;
    projectile.max_lifetime_ticks = 1;
    world.registry().emplace<network_example::OnExpiredTriggerTag>(
        *source_entity);

    std::vector<KernelEvent> events;
    network_example::simulate_projectiles(world, 0.05f, 1, &events);

    require(!world.find_entity(source).has_value());
    require(count_events(events, KernelEventType_EntitySpawned) == 1);
    for (const KernelEvent& event : events) {
        if (event.type != KernelEventType_EntitySpawned) {
            continue;
        }
        const auto spawned_entity = world.find_entity(event.net_id);
        require(spawned_entity.has_value());
        require(world.registry()
                    .get<network_example::ProjectileState>(*spawned_entity)
                    .projectile_template_id == 9);
    }
}

}  // namespace

int main() {
    matching_projectiles_destroy_without_damage();
    matching_interaction_spawns_area_effect();
    non_matching_weapon_ids_do_not_react();
    interaction_uses_swept_projectile_collision_geometry();
    interaction_respects_masks_and_owner_peer_exclusion();
    multiple_reactions_resolve_by_projectile_pair_order();
    action_graph_binding_validation_is_typed_and_authoritative();
    impact_response_spawns_area_effect_projectile_once();
    impact_response_is_additive_with_direct_hit_damage();
    world_impact_emits_impact_response_once();
    historical_hit_emits_impact_response_once();
    expired_response_does_not_reuse_impact_response();
    return 0;
}
