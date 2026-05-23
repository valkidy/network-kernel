#include "world/public/world.h"

#include "gameplay/public/gameplay_data.h"

namespace network_example {

NetId World::spawn_player(PeerId owner_peer, const glm::vec3& position) {
    const GameplayTuning tuning = default_gameplay_tuning();
    const entt::entity entity =
        create_networked_entity(EntityType::kPlayer, owner_peer, position);
    registry().emplace<PlayerTag>(entity);
    registry().emplace<Velocity>(entity);
    registry().emplace<Health>(entity, tuning.archetypes.player.health);
    registry().emplace<WeaponState>(
        entity,
        default_weapon_state(tuning.weapon_catalog));
    registry().emplace<Hitbox>(entity, tuning.archetypes.player.hitbox);
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_enemy(const glm::vec3& position) {
    const GameplayTuning tuning = default_gameplay_tuning();
    const entt::entity entity = create_networked_entity(EntityType::kEnemy, 0, position);
    registry().emplace<EnemyTag>(entity);
    registry().emplace<Velocity>(entity);
    registry().emplace<Health>(entity, tuning.archetypes.enemy.health);
    registry().emplace<Hitbox>(entity, tuning.archetypes.enemy.hitbox);
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_projectile(
    PeerId owner_peer,
    const glm::vec3& position,
    const glm::vec3& velocity) {
    const entt::entity entity =
        create_networked_entity(EntityType::kProjectile, owner_peer, position);
    const GameplayTuning tuning = default_gameplay_tuning();
    registry().emplace<ProjectileTag>(entity);
    registry().emplace<Velocity>(entity, Velocity{velocity});
    registry().emplace<Hitbox>(entity, tuning.archetypes.projectile.hitbox);
    registry().emplace<ProjectileState>(
        entity,
        ProjectileState{
            0,
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
