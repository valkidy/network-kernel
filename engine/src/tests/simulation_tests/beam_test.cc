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
    health(world, enemy) = network_example::Health{100, 100};
    return enemy;
}

void beam_damages_targets_with_dps_accumulator() {
    network_example::World world;
    const network_example::NetId shooter =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        spawn_enemy(world, glm::vec3{2.0f, 0.0f, 0.0f});
    const network_example::NetId beam =
        world.spawn_beam(
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            5.0f,
            0.25f,
            30,
            10,
            5,
            network_example::kCollisionLayerEnemy);
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
    world.spawn_beam(
        1,
        shooter,
        glm::vec3{0.0f, 0.5f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        4.0f,
        0.25f,
        30,
        10,
        5,
        network_example::kCollisionLayerEnemy);

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
        world.spawn_beam(
            1,
            shooter,
            glm::vec3{0.0f, 0.5f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            5.0f,
            0.25f,
            30,
            3,
            5,
            network_example::kCollisionLayerEnemy);

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
            network_example::WeaponFireMode::kBeam,
            2,
            20,
            1,
            10,
        };
    tuning.definitions[network_example::kWeaponSlot5].beam_length = 6.0f;
    tuning.definitions[network_example::kWeaponSlot5].beam_radius = 0.25f;
    tuning.definitions[network_example::kWeaponSlot5].beam_damage_per_second = 30;
    tuning.definitions[network_example::kWeaponSlot5].beam_lifetime_ticks = 2;
    tuning.definitions[network_example::kWeaponSlot5].beam_collision_mask =
        network_example::kCollisionLayerEnemy;

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
            event.code == static_cast<std::uint32_t>(network_example::EntityType::kBeam)) {
            beam = event.net_id;
        }
    }
    require(beam != 0);
    const auto beam_entity = world.find_entity(beam);
    require(beam_entity.has_value());
    const network_example::BeamState& state =
        world.registry().get<network_example::BeamState>(*beam_entity);
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
