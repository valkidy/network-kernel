#include "world/public/world.h"

namespace network_example {

NetId World::spawn_player(PeerId owner_peer, const glm::vec3& position) {
    const entt::entity entity =
        create_networked_entity(EntityType::kPlayer, owner_peer, position);
    registry().emplace<PlayerTag>(entity);
    registry().emplace<Velocity>(entity);
    registry().emplace<Health>(entity, Health{100, 100});
    registry().emplace<WeaponState>(entity);
    registry().emplace<Hitbox>(entity, Hitbox{{0.0f, 0.9f, 0.0f}, {0.35f, 0.9f, 0.35f}, 0});
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_enemy(const glm::vec3& position) {
    const entt::entity entity = create_networked_entity(EntityType::kEnemy, 0, position);
    registry().emplace<EnemyTag>(entity);
    registry().emplace<Velocity>(entity);
    registry().emplace<Health>(entity, Health{50, 50});
    registry().emplace<Hitbox>(entity, Hitbox{{0.0f, 0.8f, 0.0f}, {0.4f, 0.8f, 0.4f}, 0});
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_projectile(
    PeerId owner_peer,
    const glm::vec3& position,
    const glm::vec3& velocity) {
    const entt::entity entity =
        create_networked_entity(EntityType::kProjectile, owner_peer, position);
    registry().emplace<ProjectileTag>(entity);
    registry().emplace<Velocity>(entity, Velocity{velocity});
    registry().emplace<Hitbox>(entity, Hitbox{{0.0f, 0.0f, 0.0f}, {0.1f, 0.1f, 0.1f}, 0});
    registry().emplace<ProjectileState>(
        entity,
        ProjectileState{
            0,
            0,
            0,
            0,
            ProjectileMotionModel::kLinear,
            0.0f,
            0.0f,
            0.0f,
            position,
            velocity,
            glm::vec3{0.0f, 0.0f, 0.0f},
            position});
    return registry().get<NetworkIdentity>(entity).net_id;
}

}  // namespace network_example
