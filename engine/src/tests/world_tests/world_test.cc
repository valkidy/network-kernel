#include <cassert>

#include <glm/glm.hpp>

#include "world/public/world.h"

int main() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{1.0f, 0.0f, 0.0f});
    assert(player == 1);
    assert(enemy == 2);
    assert(world.destroy(player));
    const network_example::NetId next =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    assert(next == 3);
    assert(!world.find_entity(player).has_value());
    assert(world.find_entity(next).has_value());
    assert(world.destroy(enemy));
    assert(!world.find_entity(enemy).has_value());
    assert(!world.destroy(enemy));
    return 0;
}
