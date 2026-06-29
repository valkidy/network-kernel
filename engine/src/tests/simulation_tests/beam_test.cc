#include <cassert>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

#include "simulation/public/simulation.h"

namespace {

void require_impl(bool condition, int line) {
    if (!condition) {
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
    std::uint16_t damage_per_second,
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
    projectile.damage = damage_per_second;
    projectile.shooter_net_id = shooter_net_id;
    projectile.collision_mask = collision_mask;
    projectile.max_lifetime_seconds = 0.0f;
    world.registry().emplace<network_example::ProjectileBeamRuntime>(
        *entity,
        network_example::ProjectileBeamRuntime{
            shooter_net_id,
            origin,
            direction,
            length,
            radius,
            damage_per_second,
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
    projectile_template.damage = 30;
    projectile_template.lifetime_ticks = 2;
    projectile_template.lifetime_seconds = 0.0f;
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
            30,
            10,
            5,
            network_example::kCollisionLayerHostileSide);
    (void)beam;

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
        30,
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
            30,
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
    weapon.ammo[network_example::kWeaponSlot5] = 2;
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*world.find_entity(player));
    tuning.configured[network_example::kWeaponSlot5] = true;
    tuning.definitions[network_example::kWeaponSlot5] =
        network_example::WeaponMechanicsDefinition{
            network_example::kWeaponSlot5,
            network_example::WeaponFireMode::kProjectile,
            2,
            20,
            1,
            10,
        };
    tuning.definitions[network_example::kWeaponSlot5].projectile_template_id = 55;
    world.set_projectile_templates({beam_template(55, network_example::kWeaponSlot5)});

    PlayerInput input{};
    input.buttons = InputButton_Fire;
    input.selected_weapon = network_example::kWeaponSlot5;
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
    require(state.damage_per_second == 30);
    require(state.expire_tick == 3);
}

}  // namespace

int main() {
    beam_damages_targets_with_dps_accumulator();
    beam_respects_range_radius_and_collision_mask();
    beam_expires_when_not_refreshed();
    beam_fire_spawns_or_refreshes_server_beam();
    return 0;
}
