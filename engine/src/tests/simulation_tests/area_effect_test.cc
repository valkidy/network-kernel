#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
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

}  // namespace

int main() {
    area_effect_damages_only_targets_inside_radius();
    area_effect_respects_per_target_damage_interval();
    server_owned_area_effect_uses_player_damage_grace();
    area_effect_expires_at_expire_tick();
    area_effect_damage_order_is_deterministic();
    area_effect_reaches_its_own_shooter_only_when_authored();
    area_effect_spares_cover_on_a_side_it_does_not_attack();
    return 0;
}
