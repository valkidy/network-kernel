#include <cassert>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
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

network_example::NetId spawn_area_projectile(
    network_example::World& world,
    network_example::PeerId owner_peer,
    const glm::vec3& position,
    float radius,
    std::uint32_t damage_interval_ticks,
    std::uint32_t expire_tick,
    std::uint16_t damage_per_interval,
    std::uint8_t source_code) {
    const network_example::NetId net_id =
        world.spawn_projectile(owner_peer, position, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*entity);
    projectile.weapon_id = source_code;
    projectile.damage = damage_per_interval;
    projectile.collision_mask = network_example::kCollisionMaskDamageable;
    projectile.max_lifetime_ticks = 0;
    world.registry().replace<network_example::Hitbox>(
        *entity,
        network_example::Hitbox{{0.0f, 0.0f, 0.0f}, {radius, radius, radius}, 0});
    world.registry().emplace<network_example::ProjectileAreaEffectRuntime>(
        *entity,
        network_example::ProjectileAreaEffectRuntime{
            radius,
            damage_per_interval,
            damage_interval_ticks,
            expire_tick,
            source_code,
            network_example::kCollisionMaskDamageable,
            network_example::ProjectileDamageFalloff::kNone,
            {},
        });
    return net_id;
}

void area_effect_damages_only_targets_inside_radius() {
    network_example::World world;
    const network_example::NetId inside = spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId outside = spawn_enemy(world, glm::vec3{5.0f, 0.0f, 0.0f});
    spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
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
    spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
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
    spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
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
        spawn_area_projectile(
            world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 3, 20, 7);
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
    spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 3.0f, 1, 0, 10, 9);
    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;

    network_example::simulate_area_effects(world, 0, &events, &pipeline);
    pipeline.confirm_ready(world, 0, 0, &events);

    require(events.size() == 6);
    require(events[0].type == KernelEventType_HitConfirmed);
    require(events[0].net_id == first);
    require(events[2].type == KernelEventType_HealthChanged);
    require(events[2].net_id == first);
    require(events[2].health_delta == -10);
    require(events[3].type == KernelEventType_HitConfirmed);
    require(events[3].net_id == second);
    require(events[5].type == KernelEventType_HealthChanged);
    require(events[5].net_id == second);
    require(events[5].health_delta == -10);
}

// A deployable: Health plus a side. World's own standalone collision world only
// registers actors, so the prop has to be placed in a supplied world by hand --
// as kStaticObstacle carrying the `damageable` category ice_block_hitbox.yaml
// authors, which is what makes cover solid to every side.
network_example::NetId spawn_cover_prop(
    network_example::World& world,
    network_example::physics::PhysicsWorld& physics,
    const glm::vec3& position,
    std::uint32_t side,
    std::uint32_t collider_id) {
    const network_example::NetId net_id = world.spawn_entity(
        network_example::EntityType::kProp,
        network_example::ActorType::kUnknown,
        0,
        position);
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    world.registry().emplace_or_replace<network_example::Health>(
        *entity, network_example::Health{100, 100});
    world.registry().emplace_or_replace<network_example::PropWorldMode>(
        *entity,
        network_example::PropWorldMode{network_example::PropMode::kPlaced});
    world.registry().emplace_or_replace<network_example::GameplaySide>(
        *entity, network_example::GameplaySide{side});

    network_example::physics::CollisionObjectDescriptor object;
    object.identity = network_example::physics::CollisionObjectIdentity{
        net_id,
        collider_id,
        network_example::physics::kHitZoneUnscaled,
        network_example::physics::CollisionObjectKind::kStaticObstacle,
        network_example::physics::CollisionLayer::kStaticObstacle,
    };
    object.identity.gameplay_category = network_example::kCollisionMaskDamageable;
    object.shape.type = network_example::physics::CollisionShapeType::kBox;
    object.shape.half_extents = glm::vec3{0.5f, 0.5f, 0.5f};
    object.position = position;
    std::string error;
    require(physics.upsert_object(object, &error));
    return net_id;
}

// The sweep is what stops a travelling field at a wall, and it is not run at all
// unless the template authored something that stops it. Zero, the default, is
// both the old behaviour and the reason nothing that does not need this pays
// for it.
void a_travelling_area_effect_is_swept_only_when_it_authored_a_motion_mask() {
    const auto x_after_two_ticks = [](std::uint32_t motion_collision_mask) {
        network_example::World world;
        network_example::physics::PhysicsWorld physics;
        world.set_collision_world(&physics);

        // A wall across the path, 1 m out.
        network_example::physics::CollisionObjectDescriptor wall;
        wall.identity = network_example::physics::CollisionObjectIdentity{
            0,
            9001,
            network_example::physics::kHitZoneUnscaled,
            network_example::physics::CollisionObjectKind::kTerrain,
            network_example::physics::CollisionLayer::kTerrain,
        };
        wall.shape.type = network_example::physics::CollisionShapeType::kBox;
        wall.shape.half_extents = glm::vec3{0.25f, 2.0f, 4.0f};
        wall.position = glm::vec3{1.0f, 0.5f, 0.0f};
        std::string error;
        require(physics.upsert_object(wall, &error));

        const network_example::NetId area = spawn_area_projectile(
            world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
        const auto entity = world.find_entity(area);
        require(entity.has_value());
        network_example::ProjectileState& projectile =
            world.registry().get<network_example::ProjectileState>(*entity);
        projectile.spawn_position = glm::vec3{0.0f, 0.5f, 0.0f};
        projectile.initial_velocity = glm::vec3{30.0f, 0.0f, 0.0f};
        projectile.collision_geometry.shape_type =
            network_example::ColliderShapeType::kSphere;
        projectile.collision_geometry.radius = 0.1f;
        projectile.has_collision_geometry = true;
        world.registry()
            .get<network_example::ProjectileAreaEffectRuntime>(*entity)
            .motion_collision_mask = motion_collision_mask;

        network_example::simulate_projectiles(world, 1.0f / 30.0f);
        network_example::simulate_projectiles(world, 1.0f / 30.0f);
        return world.registry()
            .get<network_example::Transform>(*entity)
            .position.x;
    };

    // 30 m/s for two ticks is 2 m, so an unswept field is well past the wall.
    require(x_after_two_ticks(0u) > 1.9f);
    // Swept, it stops short of the wall's near face.
    const float stopped = x_after_two_ticks(KERNEL_COLLISION_LAYER_TERRAIN);
    require(stopped > 0.0f);
    require(stopped < 1.0f);
}

// An area effect used to be pinned where it spawned: the loader dropped any
// authored speed and the spawn path overwrote its velocity with zero. It can
// now travel, which is what a field that sweeps across the ground needs. A
// blast authors no speed and so still costs the ageing loop nothing.
void a_travelling_area_effect_advances_and_a_still_one_does_not() {
    const auto x_after_two_ticks = [](float speed) {
        network_example::World world;
        network_example::RuntimeProjectileTemplate area_template{};
        area_template.projectile_template_id = 41;
        area_template.projectile_type = network_example::ProjectileType::kAreaEffect;
        area_template.motion_model = network_example::ProjectileMotionModel::kLinear;
        area_template.speed = speed;
        area_template.area_radius = 2.0f;
        area_template.damage = 20;
        area_template.damage_interval_ticks = 10;
        area_template.lifetime_ticks = 30;
        area_template.collision_mask = network_example::kCollisionMaskDamageable;
        world.set_projectile_templates({area_template});

        require(network_example::spawn_action_graph_projectile(
            world,
            41,
            0,
            0,
            0,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            0,
            1.0f / 30.0f));

        network_example::NetId area = 0;
        for (const entt::entity entity :
             world.registry().view<
                 network_example::NetworkIdentity,
                 network_example::ProjectileAreaEffectRuntime>()) {
            area = world.registry()
                       .get<network_example::NetworkIdentity>(entity)
                       .net_id;
        }
        require(area != 0);

        network_example::simulate_projectiles(world, 1.0f / 30.0f);
        network_example::simulate_projectiles(world, 1.0f / 30.0f);

        const auto entity = world.find_entity(area);
        require(entity.has_value());
        return world.registry()
            .get<network_example::Transform>(*entity)
            .position.x;
    };

    require(x_after_two_ticks(0.0f) == 0.0f);
    // Two ticks at 6 m/s on a 30 Hz clock.
    const float travelled = x_after_two_ticks(6.0f);
    require(travelled > 0.39f && travelled < 0.41f);
}

// The overlap query filters the shooter out, which is why a weapon's own blast
// has never been able to push or hurt the actor that fired it. hit_instigator is
// how a template asks for the opposite, and it buys self-damage along with the
// self-knockback because one query feeds both.
void area_effect_reaches_its_own_shooter_only_when_authored() {
    const auto shooter_hp_after_blast = [](bool hit_instigator) {
        network_example::World world;
        const network_example::NetId shooter =
            spawn_enemy(world, glm::vec3{0.5f, 0.0f, 0.0f});
        const network_example::NetId bystander =
            spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
        const network_example::NetId area = spawn_area_projectile(
            world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 2.0f, 10, 0, 20, 7);
        const auto area_entity = world.find_entity(area);
        require(area_entity.has_value());
        world.registry()
            .get<network_example::ProjectileState>(*area_entity)
            .shooter_net_id = shooter;
        world.registry()
            .get<network_example::ProjectileAreaEffectRuntime>(*area_entity)
            .hit_instigator = hit_instigator;

        network_example::DamagePipeline pipeline;
        std::vector<KernelEvent> events;
        network_example::simulate_area_effects(world, 0, &events, &pipeline);
        pipeline.confirm_ready(world, 0, 0, &events);

        // Whoever else is standing in it is hit either way; only the shooter's
        // treatment is what this switch decides.
        require(health(world, bystander).hp == 30);
        return health(world, shooter).hp;
    };

    require(shooter_hp_after_blast(false) == 50);
    require(shooter_hp_after_blast(true) == 30);
}

void area_effect_spares_cover_on_a_side_it_does_not_attack() {
    // Splash used to level a deployable regardless of whose it was, so a rocket
    // cleared the thrower's own cover while a beam through the same block could
    // not. The rule now lives in the shared damage path, so both agree.
    //
    // Both props look identical to the collision filter here -- World's
    // standalone collision world tags everything carrying a Hitbox as an actor
    // hitbox and derives the category from actor_type -- so reaching them is not
    // what separates them. Only GameplaySide does, which is the point.
    network_example::World world;
    network_example::physics::PhysicsWorld physics;
    world.set_collision_world(&physics);
    const network_example::NetId friendly_cover = spawn_cover_prop(
        world,
        physics,
        glm::vec3{1.0f, 0.0f, 0.0f},
        network_example::kCollisionLayerPlayerSide,
        930);
    const network_example::NetId hostile_cover = spawn_cover_prop(
        world,
        physics,
        glm::vec3{-1.0f, 0.0f, 0.0f},
        network_example::kCollisionLayerHostileSide,
        931);
    const network_example::NetId net_id = spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 3.0f, 10, 0, 20, 7);
    const auto projectile_entity = world.find_entity(net_id);
    assert(projectile_entity.has_value());
    // Authored to attack hostiles, and allowed to touch props at all.
    world.registry()
        .get<network_example::ProjectileAreaEffectRuntime>(*projectile_entity)
        .collision_mask =
        network_example::kCollisionLayerHostileSide | KERNEL_COLLISION_MASK_PROP;

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    network_example::simulate_area_effects(world, 0, &events, &pipeline);
    pipeline.confirm_ready(world, 0, 0, &events);

    require(health(world, hostile_cover).hp == 80);
    require(health(world, friendly_cover).hp == 100);
}

// An area effect that carries an action graph takes a different branch than one
// that only deals damage: it queues one trigger per target and hands the batches
// back to the caller instead of submitting damage requests. Nothing covered that
// branch, so "splash only pushed one unit" had no test that could tell a
// single-target *query* apart from a single-target *dispatch*.
network_example::CompiledActionGraphBinding impulse_on_impact_binding(
    float strength,
    std::uint8_t direction_source = KernelEventVec3Source_Direction) {
    KernelActionTriggerDefinition trigger{};
    trigger.struct_size = sizeof(trigger);
    trigger.action_type = KernelEntityTriggerActionType_ApplyImpulse;
    trigger.target_source = KernelEntityRefSource_EventTarget;
    trigger.direction_source = direction_source;
    trigger.impulse_strength = strength;
    trigger.impulse_collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    std::optional<network_example::CompiledActionGraphBinding> binding =
        network_example::compile_action_trigger_definition(
            network_example::TriggerEventType::kProjectileImpact, trigger);
    require(binding.has_value());
    return *binding;
}

void bind_impulse_graph(
    network_example::World& world,
    network_example::NetId area_net_id,
    float strength,
    std::uint8_t direction_source = KernelEventVec3Source_Direction) {
    const auto entity = world.find_entity(area_net_id);
    require(entity.has_value());
    world.registry()
        .get<network_example::ProjectileAreaEffectRuntime>(*entity)
        .action_graph_binding =
            impulse_on_impact_binding(strength, direction_source);
}

const network_example::ActionApplyImpulseCommand& only_impulse_command(
    const network_example::ActionGraphCommandBatch& batch) {
    require(batch.commands.size() == 1);
    const auto* impulse =
        std::get_if<network_example::ActionApplyImpulseCommand>(
            &batch.commands.front());
    require(impulse != nullptr);
    return *impulse;
}

void area_effect_dispatches_its_graph_once_per_target_in_radius() {
    network_example::World world;
    const network_example::NetId first =
        spawn_enemy(world, glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId second =
        spawn_enemy(world, glm::vec3{-2.0f, 0.0f, 0.0f});
    const network_example::NetId outside =
        spawn_enemy(world, glm::vec3{9.0f, 0.0f, 0.0f});
    const network_example::NetId area = spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 4.0f, 45, 0, 45, 7);
    bind_impulse_graph(world, area, 12.0f);

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    std::vector<network_example::ActionGraphCommandBatch> batches;
    network_example::simulate_area_effects(
        world, 0, 0, &events, &pipeline, &batches);

    // One batch per target inside the radius, and none for the one outside it.
    require(batches.size() == 2);
    const network_example::ActionApplyImpulseCommand& first_impulse =
        only_impulse_command(batches[0]);
    const network_example::ActionApplyImpulseCommand& second_impulse =
        only_impulse_command(batches[1]);
    require(first_impulse.target != second_impulse.target);
    require(first_impulse.target == first || first_impulse.target == second);
    require(second_impulse.target == first || second_impulse.target == second);
    require(first_impulse.target != outside);
    require(second_impulse.target != outside);
    require(first_impulse.strength == 12.0f);
    require(second_impulse.strength == 12.0f);

    // The graph branch replaces the damage branch outright: neither target is
    // in the damage pipeline, so a graph that forgets apply_damage deals none.
    require(pipeline.pending_count() == 0);
    require(health(world, first).hp == 50);
    require(health(world, second).hp == 50);
}

// `direction` is radial, so it is a different vector for every target one blast
// reports. `subject_direction` is the field's own heading, so it is the same
// vector for all of them -- which is what a front that sweeps across the ground
// needs, and what a blast standing still has none of.
void a_travelling_area_effect_reports_its_own_heading() {
    const auto impulse_direction_for = [](std::uint8_t direction_source) {
        network_example::World world;
        // Off the travel axis on purpose: the radial push is +Z here, so the
        // two sources cannot be confused for one another.
        const network_example::NetId target =
            spawn_enemy(world, glm::vec3{0.0f, 0.0f, 2.0f});
        const network_example::NetId area = spawn_area_projectile(
            world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 4.0f, 45, 0, 45, 7);
        const auto entity = world.find_entity(area);
        require(entity.has_value());
        // Travelling along +X.
        world.registry()
            .get<network_example::ProjectileState>(*entity)
            .initial_velocity = glm::vec3{6.0f, 0.0f, 0.0f};
        bind_impulse_graph(world, area, 12.0f, direction_source);

        network_example::DamagePipeline pipeline;
        std::vector<KernelEvent> events;
        std::vector<network_example::ActionGraphCommandBatch> batches;
        network_example::simulate_area_effects(
            world, 0, 0, &events, &pipeline, &batches);
        require(batches.size() == 1);
        const network_example::ActionApplyImpulseCommand& impulse =
            only_impulse_command(batches.front());
        require(impulse.target == target);
        return impulse.direction;
    };

    const glm::vec3 radial = impulse_direction_for(KernelEventVec3Source_Direction);
    require(radial.z > 0.9f);
    require(std::fabs(radial.x) < 0.1f);

    const glm::vec3 heading =
        impulse_direction_for(KernelEventVec3Source_SubjectDirection);
    require(heading.x > 0.9f);
    require(std::fabs(heading.z) < 0.1f);
}

// Where the push points, for a blast whose centre sits at the same height as
// what it hits. This is not a preference, it is what the radial direction
// geometrically is -- and it is the half of the knockback the movement solver
// then throws away for anything that submits input.
void area_effect_impulse_direction_is_level_for_a_level_blast() {
    network_example::World world;
    const network_example::NetId east =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.0f});
    const network_example::NetId area = spawn_area_projectile(
        world, 0, glm::vec3{0.0f, 0.5f, 0.0f}, 4.0f, 45, 0, 45, 7);
    bind_impulse_graph(world, area, 12.0f);

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    std::vector<network_example::ActionGraphCommandBatch> batches;
    network_example::simulate_area_effects(
        world, 0, 0, &events, &pipeline, &batches);

    require(batches.size() == 1);
    const network_example::ActionApplyImpulseCommand& impulse =
        only_impulse_command(batches[0]);
    require(impulse.target == east);
    require(impulse.direction.x > 0.9f);
    require(std::abs(impulse.direction.y) < 0.05f);
}

}  // namespace

int main() {
    area_effect_damages_only_targets_inside_radius();
    area_effect_respects_per_target_damage_interval();
    server_owned_area_effect_uses_player_damage_grace();
    area_effect_expires_at_expire_tick();
    area_effect_damage_order_is_deterministic();
    a_travelling_area_effect_advances_and_a_still_one_does_not();
    a_travelling_area_effect_is_swept_only_when_it_authored_a_motion_mask();
    area_effect_reaches_its_own_shooter_only_when_authored();
    area_effect_spares_cover_on_a_side_it_does_not_attack();
    area_effect_dispatches_its_graph_once_per_target_in_radius();
    a_travelling_area_effect_reports_its_own_heading();
    area_effect_impulse_direction_is_level_for_a_level_blast();
    return 0;
}
