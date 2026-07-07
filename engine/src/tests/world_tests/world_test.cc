#include <cassert>

#include <glm/glm.hpp>

#include "world/public/world.h"

int main() {
    network_example::World world;
    const network_example::NetId player =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    const network_example::NetId enemy =
        world.spawn_enemy(glm::vec3{1.0f, 0.0f, 0.0f});
    const network_example::NetId projectile =
        world.spawn_projectile(
            1,
            glm::vec3{2.0f, 0.0f, 0.0f},
            glm::vec3{5.0f, 0.0f, 0.0f});
    assert(player == 1);
    assert(enemy == 2);
    assert(projectile == 3);
    const auto projectile_entity = world.find_entity(projectile);
    assert(projectile_entity.has_value());
    assert(
        world.registry().get<network_example::EntityKind>(*projectile_entity).type ==
        network_example::EntityType::kProjectile);
    assert(world.registry().all_of<network_example::ProjectileTag>(*projectile_entity));
    static_assert(
        network_example::kCollisionMaskDamageable ==
        (network_example::kCollisionLayerPlayerSide |
         network_example::kCollisionLayerHostileSide |
         network_example::kCollisionLayerNeutral));
    assert(world.destroy(player));
    const network_example::NetId next =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    assert(next == 4);
    assert(!world.find_entity(player).has_value());
    assert(world.find_entity(next).has_value());
    assert(world.destroy(enemy));
    assert(!world.find_entity(enemy).has_value());
    assert(!world.destroy(enemy));
    return 0;
}
