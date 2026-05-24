#include <cassert>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

int main() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy = world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const auto player_entity = world.find_entity(player);
    assert(player_entity.has_value());
    network_example::WeaponState& weapon =
        world.registry().get<network_example::WeaponState>(*player_entity);
    weapon.ammo[network_example::kWeaponSlot0] = 30;
    network_example::WeaponTuning& tuning =
        world.registry().get<network_example::WeaponTuning>(*player_entity);
    tuning.configured[network_example::kWeaponSlot0] = true;
    tuning.definitions[network_example::kWeaponSlot0] =
        network_example::WeaponMechanicsDefinition{
            network_example::kWeaponSlot0,
            network_example::WeaponFireMode::kHitscan,
            30,
            25,
            3,
            30,
            100.0f};
    world.registry().get<network_example::Hitbox>(*player_entity) =
        network_example::Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0};
    const auto enemy_entity = world.find_entity(enemy);
    assert(enemy_entity.has_value());
    world.registry().get<network_example::Health>(*enemy_entity) =
        network_example::Health{50, 50};
    world.registry().get<network_example::Hitbox>(*enemy_entity) =
        network_example::Hitbox{{0.0f, 0.8f, 0.0f}, {0.4f, 0.8f, 0.4f}, 0};

    PlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire;

    std::vector<network_example::QueuedInput> inputs = {
        network_example::QueuedInput{1, input},
    };
    std::vector<KernelEvent> events;
    network_example::simulate_hitscan_weapons(world, inputs, 0, &events);

    const network_example::Health& health =
        world.registry().get<network_example::Health>(*enemy_entity);
    assert(health.hp == 25);
    assert(events.size() == 3);
    return 0;
}
