#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"

namespace {

void require_impl(bool condition, int line) {
    if (!condition) {
        // Named, because an assertion that aborts in silence tells whoever
        // broke it nothing at all.
        std::fprintf(stderr, "require failed at line %d\n", line);
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

network_example::NetId spawn_enemy(
    network_example::World& world,
    const glm::vec3& position) {
    const network_example::NetId enemy = world.spawn_enemy(position);
    health(world, enemy) = network_example::Health{100, 100};
    return enemy;
}

network_example::NetId spawn_projectile_beam(
    network_example::World& world,
    network_example::PeerId owner_peer,
    network_example::NetId shooter_net_id,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float length,
    float radius,
    std::uint16_t damage_per_tick,
    std::uint32_t expire_tick,
    std::uint8_t source_code,
    std::uint32_t collision_mask) {
    const network_example::NetId net_id =
        world.spawn_projectile(owner_peer, origin, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    network_example::ProjectileState& projectile =
        world.registry().get<network_example::ProjectileState>(*entity);
    projectile.weapon_id = source_code;
    projectile.damage = damage_per_tick;
    projectile.shooter_net_id = shooter_net_id;
    projectile.collision_mask = collision_mask;
    projectile.max_lifetime_ticks = 0;
    world.registry().emplace<network_example::ProjectileBeamRuntime>(
        *entity,
        network_example::ProjectileBeamRuntime{
            shooter_net_id,
            origin,
            direction,
            length,
            radius,
            damage_per_tick,
            expire_tick,
            source_code,
            collision_mask,
            {},
        });
    return net_id;
}

network_example::RuntimeProjectileTemplate beam_template(
    std::uint32_t template_id,
    std::uint8_t weapon_id) {
    network_example::RuntimeProjectileTemplate projectile_template;
    projectile_template.projectile_template_id = template_id;
    projectile_template.weapon_id = weapon_id;
    projectile_template.projectile_type = network_example::ProjectileType::kBeam;
    projectile_template.motion_model = network_example::ProjectileMotionModel::kLinear;
    projectile_template.speed = 0.0f;
    projectile_template.damage = 1;
    projectile_template.lifetime_ticks = 2;
    projectile_template.beam_length = 6.0f;
    projectile_template.beam_radius = 0.25f;
    projectile_template.collision_mask = network_example::kCollisionLayerHostileSide;
    return projectile_template;
}

void beam_damages_targets_with_dps_accumulator() {
    network_example::World world;
    const network_example::NetId shooter =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.0f});
    const network_example::NetId beam =
        spawn_projectile_beam(
            world,
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            5.0f,
            0.25f,
            1,
            10,
            5,
            network_example::kCollisionLayerHostileSide);
    assert(beam != 0);

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    network_example::simulate_beams(world, 1, 1.0f / 30.0f, 33333, &events, &pipeline);
    std::vector<network_example::ConfirmedDamage> ready =
        pipeline.drain_ready_damage(world, 33333);
    require(ready.size() == 1);
    require(ready[0].target_net_id == enemy);
    require(ready[0].damage == 1);
    network_example::apply_damage_applications(world, ready, 1, &events);
    require(health(world, enemy).hp == 99);
}

void beam_respects_range_radius_and_collision_mask() {
    network_example::World world;
    const network_example::NetId shooter =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId in_beam =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.2f});
    const network_example::NetId outside_radius =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.9f});
    const network_example::NetId outside_range =
        spawn_enemy(world, glm::vec3{6.0f, 0.0f, 0.0f});
    const network_example::NetId friendly =
        world.spawn_player(2, glm::vec3{2.5f, 0.0f, 0.0f});
    health(world, friendly) = network_example::Health{100, 100};
    spawn_projectile_beam(
        world,
        1,
        shooter,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        4.0f,
        0.25f,
        1,
        10,
        5,
        network_example::kCollisionLayerHostileSide);

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    network_example::simulate_beams(world, 1, 1.0f / 30.0f, 33333, &events, &pipeline);
    const std::vector<network_example::ConfirmedDamage> ready =
        pipeline.drain_ready_damage(world, 33333);
    require(ready.size() == 1);
    require(ready[0].target_net_id == in_beam);
    network_example::apply_damage_applications(world, ready, 1, &events);
    require(health(world, in_beam).hp == 99);
    require(health(world, outside_radius).hp == 100);
    require(health(world, outside_range).hp == 100);
    require(health(world, friendly).hp == 100);
}

void beam_expires_when_not_refreshed() {
    network_example::World world;
    const network_example::NetId shooter =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId beam =
        spawn_projectile_beam(
            world,
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            5.0f,
            0.25f,
            1,
            3,
            5,
            network_example::kCollisionLayerHostileSide);

    std::vector<KernelEvent> events;
    network_example::simulate_beams(world, 3, 1.0f / 30.0f, 100000, &events, nullptr);
    require(!world.find_entity(beam).has_value());
}

void beam_fire_spawns_or_refreshes_server_beam() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    network_example::Health& player_health = health(world, player);
    player_health = network_example::Health{100, 100};
    network_example::WeaponState& weapon =
        world.registry().get<network_example::WeaponState>(*world.find_entity(player));
    weapon.weapon_slot_count = 1;
    weapon.weapon_ids[0] = network_example::kWeaponId5;
    weapon.ammo[0] = 2;
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*world.find_entity(player));
    tuning.configured[network_example::kWeaponId5] = true;
    tuning.definitions[network_example::kWeaponId5] =
        network_example::WeaponMechanicsDefinition{
            network_example::kWeaponId5,
            network_example::WeaponFireMode::kProjectile,
            2,
            20,
            1,
            10,
        };
    tuning.definitions[network_example::kWeaponId5].projectile_template_id = 55;
    world.set_projectile_templates({beam_template(55, network_example::kWeaponId5)});

    KernelPlayerInput input{};
    input.buttons = InputButton_Fire;
    input.selected_weapon = network_example::kWeaponId5;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    std::vector<network_example::QueuedInput> inputs{
        network_example::QueuedInput{1, input, 0, 0, false, 0}};
    std::vector<KernelEvent> events;
    network_example::simulate_weapons(world, inputs, 1, &events);

    network_example::NetId beam = 0;
    for (const KernelEvent& event : events) {
        if (event.type == KernelEventType_EntitySpawned &&
            event.code ==
                static_cast<std::uint32_t>(network_example::EntityType::kProjectile)) {
            beam = event.net_id;
        }
    }
    require(beam != 0);
    const auto beam_entity = world.find_entity(beam);
    require(beam_entity.has_value());
    const network_example::ProjectileBeamRuntime& state =
        world.registry().get<network_example::ProjectileBeamRuntime>(*beam_entity);
    require(state.shooter_net_id == player);
    require(state.length == 6.0f);
    require(state.damage_per_tick == 1);
    require(state.expire_tick == 3);
}

void beam_survives_continuous_refresh() {
    // A held beam is one entity refreshed every tick, not a new entity every
    // tick. simulate_projectiles used to age it against max_lifetime_ticks --
    // which apply_projectile_mechanics sets from beam.lifetime_ticks -- and the
    // weapon refresh never reset age_ticks, so the beam was destroyed and
    // respawned under a fresh net_id on a two-tick cycle. At a 15 Hz snapshot
    // rate against a 30 Hz tick that reaches the client as a strobe.
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const auto player_entity = world.find_entity(player);
    assert(player_entity.has_value());
    health(world, player) = network_example::Health{100, 100};
    network_example::WeaponState& weapon =
        world.registry().get<network_example::WeaponState>(*player_entity);
    weapon.weapon_slot_count = 1;
    weapon.weapon_ids[0] = network_example::kWeaponId5;
    weapon.ammo[0] = 60;
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*player_entity);
    tuning.configured[network_example::kWeaponId5] = true;
    tuning.definitions[network_example::kWeaponId5] =
        network_example::WeaponMechanicsDefinition{
            network_example::kWeaponId5,
            network_example::WeaponFireMode::kProjectile,
            60,
            20,
            1,
            10,
        };
    tuning.definitions[network_example::kWeaponId5].projectile_template_id = 55;
    world.set_projectile_templates({beam_template(55, network_example::kWeaponId5)});

    KernelPlayerInput input{};
    input.buttons = InputButton_Fire;
    input.selected_weapon = network_example::kWeaponId5;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};

    network_example::NetId first_beam = 0;
    int spawn_count = 0;
    int destroy_count = 0;
    for (std::uint32_t tick = 1; tick <= 12; ++tick) {
        std::vector<KernelEvent> events;
        const std::vector<network_example::QueuedInput> inputs{
            network_example::QueuedInput{1, input, 0, 0, false, 0}};
        // The kernel's own order: weapons, then projectiles, then beams.
        network_example::simulate_weapons(world, inputs, tick, &events);
        network_example::simulate_projectiles(world, 1.0f / 30.0f, tick, &events);
        network_example::simulate_beams(
            world, tick, 1.0f / 30.0f, tick * 33333ull, &events, nullptr);

        for (const KernelEvent& event : events) {
            if (event.type == KernelEventType_EntitySpawned &&
                event.code == static_cast<std::uint32_t>(
                                  network_example::EntityType::kProjectile)) {
                ++spawn_count;
                if (first_beam == 0) {
                    first_beam = event.net_id;
                }
            }
            if (event.type == KernelEventType_EntityDestroyed) {
                ++destroy_count;
            }
        }

        // Exactly one live beam, and it is still the one spawned on tick 1.
        int live_beams = 0;
        auto view = world.registry().view<network_example::ProjectileBeamRuntime>();
        for (const entt::entity entity : view) {
            ++live_beams;
            require(
                world.registry().get<network_example::NetworkIdentity>(entity)
                    .net_id == first_beam);
            // Untouched by the ageing loop, which is what stops the expiry.
            require(
                world.registry().get<network_example::ProjectileState>(entity)
                    .age_ticks == 0);
        }
        require(live_beams == 1);
    }
    require(first_beam != 0);
    require(spawn_count == 1);
    require(destroy_count == 0);
    require(
        world.registry().get<network_example::WeaponState>(*player_entity)
            .active_effect_net_id == first_beam);
}

void beam_transform_rotation_follows_aim() {
    // The collider template runs its length along local +Z and the presentation
    // prefabs are built on the same axis, so the replicated transform has to
    // carry that mapping. Nothing else writes it.
    const auto rotated_forward = [](const glm::vec3& direction) {
        network_example::World world;
        const network_example::NetId shooter =
            world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
        const network_example::NetId beam = spawn_projectile_beam(
            world,
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            direction,
            5.0f,
            0.25f,
            1,
            10,
            5,
            network_example::kCollisionLayerHostileSide);
        std::vector<KernelEvent> events;
        network_example::simulate_beams(
            world, 1, 1.0f / 30.0f, 33333, &events, nullptr);
        const auto entity = world.find_entity(beam);
        assert(entity.has_value());
        const network_example::Transform& transform =
            world.registry().get<network_example::Transform>(*entity);
        return transform.rotation * glm::vec3{0.0f, 0.0f, 1.0f};
    };

    const auto close = [](const glm::vec3& lhs, const glm::vec3& rhs) {
        return glm::length(lhs - rhs) < 0.001f;
    };

    require(close(rotated_forward(glm::vec3{1.0f, 0.0f, 0.0f}),
                  glm::vec3{1.0f, 0.0f, 0.0f}));
    require(close(rotated_forward(glm::vec3{0.0f, 0.0f, 1.0f}),
                  glm::vec3{0.0f, 0.0f, 1.0f}));
    // Antiparallel and straight up are the two degenerate axes for a
    // cross-product construction; both must still produce a finite rotation.
    require(close(rotated_forward(glm::vec3{0.0f, 0.0f, -1.0f}),
                  glm::vec3{0.0f, 0.0f, -1.0f}));
    require(close(rotated_forward(glm::vec3{0.0f, 1.0f, 0.0f}),
                  glm::vec3{0.0f, 1.0f, 0.0f}));
}

// A destructible blocker: a prop carrying Health and a side, standing in the
// beam's path. Registered the way the kernel registers a prop -- kind and layer
// kStaticObstacle -- and carrying the gameplay_category ice_block_hitbox.yaml
// authors, `damageable`. That category covers all three sides on purpose, so
// blocking never depends on which side the block belongs to; only the entity's
// GameplaySide does, and only for damage.
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
    object.shape.half_extents = glm::vec3{0.3f, 1.0f, 1.0f};
    object.position = position;
    std::string error;
    require(physics.upsert_object(object, &error));
    return net_id;
}

// World only auto-registers actor hitboxes into the standalone collision world
// it builds for itself. A test that supplies its own world has to place them.
void register_actor_hitbox(
    network_example::World& world,
    network_example::physics::PhysicsWorld& physics,
    network_example::NetId net_id,
    std::uint32_t side,
    std::uint32_t collider_id) {
    const auto entity = world.find_entity(net_id);
    assert(entity.has_value());
    const network_example::Transform& transform =
        world.registry().get<network_example::Transform>(*entity);
    const network_example::Hitbox& hitbox =
        world.registry().get<network_example::Hitbox>(*entity);

    network_example::physics::CollisionObjectDescriptor object;
    object.identity = network_example::physics::CollisionObjectIdentity{
        net_id,
        collider_id,
        network_example::physics::kHitZoneUnscaled,
        network_example::physics::CollisionObjectKind::kActorHitbox,
        network_example::physics::CollisionLayer::kDamageable,
    };
    object.identity.gameplay_category = side;
    object.shape.type = network_example::physics::CollisionShapeType::kBox;
    object.shape.half_extents = hitbox.half_extents;
    object.position = transform.position + transform.rotation * hitbox.center;
    std::string error;
    require(physics.upsert_object(object, &error));
}

void beam_is_blocked_by_cover_regardless_of_side() {
    // Blocking does not consult the side. A beam authored to attack hostiles is
    // stopped by friendly cover just the same, and the actor sheltering behind
    // it takes nothing.
    for (const std::uint32_t cover_side :
         {network_example::kCollisionLayerPlayerSide,
          network_example::kCollisionLayerHostileSide}) {
        network_example::World world;
        network_example::physics::PhysicsWorld physics;
        world.set_collision_world(&physics);
        const network_example::NetId shooter =
            world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
        const network_example::NetId sheltered =
            spawn_enemy(world, glm::vec3{4.0f, 0.5f, 0.0f});
        register_actor_hitbox(
            world,
            physics,
            sheltered,
            network_example::kCollisionLayerHostileSide,
            910);
        // Control: without the cover this beam reaches the enemy, so a pass
        // below means the cover stopped it rather than the beam missing.
        {
            network_example::World control_world;
            network_example::physics::PhysicsWorld control_physics;
            control_world.set_collision_world(&control_physics);
            const network_example::NetId control_shooter =
                control_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
            const network_example::NetId reachable =
                spawn_enemy(control_world, glm::vec3{4.0f, 0.5f, 0.0f});
            register_actor_hitbox(
                control_world,
                control_physics,
                reachable,
                network_example::kCollisionLayerHostileSide,
                911);
            spawn_projectile_beam(
                control_world,
                1,
                control_shooter,
                glm::vec3{0.0f, 0.5f, 0.0f},
                glm::vec3{1.0f, 0.0f, 0.0f},
                8.0f,
                0.25f,
                1,
                10,
                5,
                network_example::kCollisionLayerHostileSide |
                    network_example::kCollisionLayerStaticObstacle);
            network_example::DamagePipeline control_pipeline;
            std::vector<KernelEvent> control_events;
            network_example::simulate_beams(
                control_world, 1, 1.0f / 30.0f, 33333, &control_events,
                &control_pipeline);
            const std::vector<network_example::ConfirmedDamage> control_ready =
                control_pipeline.drain_ready_damage(control_world, 33333);
            network_example::apply_damage_applications(
                control_world, control_ready, 1, &control_events);
            require(health(control_world, reachable).hp == 99);
        }

        spawn_cover_prop(
            world, physics, glm::vec3{2.0f, 0.5f, 0.0f}, cover_side, 900);

        spawn_projectile_beam(
            world,
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            8.0f,
            0.25f,
            1,
            10,
            5,
            network_example::kCollisionLayerHostileSide |
                network_example::kCollisionLayerStaticObstacle);

        network_example::DamagePipeline pipeline;
        std::vector<KernelEvent> events;
        network_example::simulate_beams(
            world, 1, 1.0f / 30.0f, 33333, &events, &pipeline);
        const std::vector<network_example::ConfirmedDamage> ready =
            pipeline.drain_ready_damage(world, 33333);
        network_example::apply_damage_applications(world, ready, 1, &events);
        require(health(world, sheltered).hp == 100);
    }
}

void beam_damages_only_cover_on_a_side_it_attacks() {
    const auto cover_hp_after_one_tick = [](std::uint32_t cover_side) {
        network_example::World world;
        network_example::physics::PhysicsWorld physics;
        world.set_collision_world(&physics);
        const network_example::NetId shooter =
            world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
        const network_example::NetId cover = spawn_cover_prop(
            world, physics, glm::vec3{2.0f, 0.5f, 0.0f}, cover_side, 901);

        spawn_projectile_beam(
            world,
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            8.0f,
            0.25f,
            1,
            10,
            5,
            network_example::kCollisionLayerHostileSide |
                network_example::kCollisionLayerStaticObstacle);

        network_example::DamagePipeline pipeline;
        std::vector<KernelEvent> events;
        network_example::simulate_beams(
            world, 1, 1.0f / 30.0f, 33333, &events, &pipeline);
        const std::vector<network_example::ConfirmedDamage> ready =
            pipeline.drain_ready_damage(world, 33333);
        network_example::apply_damage_applications(world, ready, 1, &events);
        return health(world, cover).hp;
    };

    // The beam attacks hostile_side, so hostile cover burns down and the
    // shooter's own cover does not.
    require(cover_hp_after_one_tick(
                network_example::kCollisionLayerHostileSide) == 99);
    require(cover_hp_after_one_tick(
                network_example::kCollisionLayerPlayerSide) == 100);
}

void beam_damages_cover_without_a_side() {
    // Un-owned destructible scenery: no side means no side is spared, matching
    // how the engine reads an absent category everywhere else. Indestructible is
    // spelled by carrying no Health at all, not by withholding a side.
    network_example::World world;
    network_example::physics::PhysicsWorld physics;
    world.set_collision_world(&physics);
    const network_example::NetId shooter =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId scenery = spawn_cover_prop(
        world, physics, glm::vec3{2.0f, 0.5f, 0.0f}, 0, 902);
    world.registry().remove<network_example::GameplaySide>(
        *world.find_entity(scenery));

    spawn_projectile_beam(
        world,
        1,
        shooter,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        8.0f,
        0.25f,
        1,
        10,
        5,
        network_example::kCollisionLayerHostileSide |
            network_example::kCollisionLayerStaticObstacle);

    network_example::DamagePipeline pipeline;
    std::vector<KernelEvent> events;
    network_example::simulate_beams(
        world, 1, 1.0f / 30.0f, 33333, &events, &pipeline);
    const std::vector<network_example::ConfirmedDamage> ready =
        pipeline.drain_ready_damage(world, 33333);
    network_example::apply_damage_applications(world, ready, 1, &events);
    require(health(world, scenery).hp == 99);
}

void beam_effective_length_stops_at_cover() {
    // What the client is told the beam reaches. Without the clip the endpoint
    // would be the authored length and presentation would draw a beam running
    // through the wall that stopped it.
    network_example::World world;
    network_example::physics::PhysicsWorld physics;
    world.set_collision_world(&physics);
    const network_example::NetId shooter =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    spawn_cover_prop(
        world,
        physics,
        glm::vec3{2.0f, 0.5f, 0.0f},
        network_example::kCollisionLayerHostileSide,
        920);
    const network_example::NetId beam = spawn_projectile_beam(
        world,
        1,
        shooter,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        8.0f,
        0.25f,
        1,
        10,
        5,
        network_example::kCollisionLayerHostileSide |
            network_example::kCollisionLayerStaticObstacle);

    std::vector<KernelEvent> events;
    network_example::simulate_beams(world, 1, 1.0f / 30.0f, 33333, &events, nullptr);
    const auto entity = world.find_entity(beam);
    assert(entity.has_value());
    const network_example::ProjectileBeamRuntime& state =
        world.registry().get<network_example::ProjectileBeamRuntime>(*entity);
    // Cover spans x in [1.7, 2.3] and the sphere cast has radius 0.25, so
    // contact lands near 1.45 -- well short of the authored 8.
    require(state.effective_length < 2.0f);
    require(state.effective_length > 1.0f);

    // Nothing in the way means the beam keeps its authored reach.
    network_example::World open_world;
    network_example::physics::PhysicsWorld open_physics;
    open_world.set_collision_world(&open_physics);
    const network_example::NetId open_shooter =
        open_world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId open_beam = spawn_projectile_beam(
        open_world,
        1,
        open_shooter,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        8.0f,
        0.25f,
        1,
        10,
        5,
        network_example::kCollisionLayerHostileSide |
            network_example::kCollisionLayerStaticObstacle);
    std::vector<KernelEvent> open_events;
    network_example::simulate_beams(
        open_world, 1, 1.0f / 30.0f, 33333, &open_events, nullptr);
    require(
        open_world.registry()
            .get<network_example::ProjectileBeamRuntime>(
                *open_world.find_entity(open_beam))
            .effective_length == 8.0f);
}

}  // namespace

int main() {
    beam_damages_targets_with_dps_accumulator();
    beam_respects_range_radius_and_collision_mask();
    beam_expires_when_not_refreshed();
    beam_fire_spawns_or_refreshes_server_beam();
    beam_survives_continuous_refresh();
    beam_transform_rotation_follows_aim();
    beam_is_blocked_by_cover_regardless_of_side();
    beam_damages_only_cover_on_a_side_it_attacks();
    beam_damages_cover_without_a_side();
    beam_effective_length_stops_at_cover();
    return 0;
}
