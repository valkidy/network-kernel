#include <cassert>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

int main() {
    network_example::World world;
    world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy = world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});

    PlayerInput input{};
    input.input_seq = 1;
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire;

    std::vector<network_example::QueuedInput> inputs = {
        network_example::QueuedInput{1, input},
    };
    std::vector<KernelEvent> events;
    network_example::simulate_hitscan_weapons(world, inputs, 0, &events);

    const auto enemy_entity = world.find_entity(enemy);
    assert(enemy_entity.has_value());
    const network_example::Health& health =
        world.registry().get<network_example::Health>(*enemy_entity);
    assert(health.hp == 25);
    assert(events.size() == 3);
    return 0;
}
