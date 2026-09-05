
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <glm/glm.hpp>

#include "world/public/world.h"

namespace {

// assert() is compiled out under -c opt, which is the configuration this suite
// runs in, so every check below was previously not being run at all.
void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

}  // namespace

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
    require(player == 1);
    require(enemy == 2);
    require(projectile == 3);
    const auto projectile_entity = world.find_entity(projectile);
    require(projectile_entity.has_value());
    require(
        world.registry().get<network_example::EntityKind>(*projectile_entity).type ==
        network_example::EntityType::kProjectile);
    require(world.registry().all_of<network_example::ProjectileTag>(*projectile_entity));
    static_assert(
        network_example::kCollisionMaskDamageable ==
        (network_example::kCollisionLayerPlayerSide |
         network_example::kCollisionLayerHostileSide |
         network_example::kCollisionLayerNeutral));
    require(world.destroy(player));
    const network_example::NetId next =
        world.spawn_player(1, glm::vec3{0.0f, 0.0f, 0.0f});
    require(next == 4);
    require(!world.find_entity(player).has_value());
    require(world.find_entity(next).has_value());
    require(world.destroy(enemy));
    require(!world.find_entity(enemy).has_value());
    require(!world.destroy(enemy));

    // Ids are never reused. Nothing records which ones have been destroyed --
    // the counter only ever advances -- so this is the property that stands in
    // for the tombstone set that used to grow by one entry per destroyed
    // entity and be read by nothing.
    std::vector<network_example::NetId> seen{player, enemy, projectile, next};
    for (int round = 0; round < 64; ++round) {
        const network_example::NetId spawned =
            world.spawn_enemy(glm::vec3{0.0f, 0.0f, 0.0f});
        for (const network_example::NetId earlier : seen) {
            require(spawned != earlier);
        }
        seen.push_back(spawned);
        require(world.destroy(spawned));
    }
    return 0;
}
