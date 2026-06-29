#include "world/public/world.h"

namespace network_example {

NetId World::spawn_player(PeerId owner_peer, const glm::vec3& position) {
    const entt::entity entity =
        create_networked_entity(
            EntityType::kActor,
            ActorType::kPlayer,
            owner_peer,
            position);
    registry().emplace<PlayerTag>(entity);
    registry().emplace<Velocity>(entity);
    registry().emplace<Health>(entity);
    registry().emplace<WeaponState>(entity);
    registry().emplace<WeaponTuning>(entity);
    registry().emplace<Hitbox>(entity);
    registry().emplace<MovementState>(entity);
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_enemy(const glm::vec3& position) {
    const entt::entity entity = create_networked_entity(
        EntityType::kActor,
        ActorType::kAgent,
        0,
        position);
    registry().emplace<AgentTag>(entity);
    registry().emplace<Velocity>(entity);
    registry().emplace<Health>(entity);
    registry().emplace<WeaponState>(entity);
    registry().emplace<WeaponTuning>(entity);
    registry().emplace<Hitbox>(entity);
    registry().emplace<MovementState>(entity);
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_projectile(
    PeerId owner_peer,
    const glm::vec3& position,
    const glm::vec3& velocity) {
    const entt::entity entity =
        create_networked_entity(
            EntityType::kProjectile,
            ActorType::kUnknown,
            owner_peer,
            position);
    registry().emplace<ProjectileTag>(entity);
    registry().emplace<Velocity>(entity, Velocity{velocity});
    registry().emplace<Hitbox>(
        entity,
        Hitbox{{0.0f, 0.0f, 0.0f}, {0.1f, 0.1f, 0.1f}, 0});
    ProjectileState& projectile = registry().emplace<ProjectileState>(entity);
    projectile.spawn_position = position;
    projectile.initial_velocity = velocity;
    projectile.previous_position = position;
    return registry().get<NetworkIdentity>(entity).net_id;
}

}  // namespace network_example
