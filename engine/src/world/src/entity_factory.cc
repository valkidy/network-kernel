#include "world/public/world.h"

namespace network_example {

NetId World::spawn_player(PeerId owner_peer, const glm::vec3& position) {
    const entt::entity entity =
        create_networked_entity(EntityType::kPlayer, owner_peer, position);
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
    const entt::entity entity = create_networked_entity(EntityType::kEnemy, 0, position);
    registry().emplace<EnemyTag>(entity);
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
        create_networked_entity(EntityType::kProjectile, owner_peer, position);
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

NetId World::spawn_area_effect(
    PeerId owner_peer,
    const glm::vec3& position,
    float radius,
    std::uint32_t damage_interval_ticks,
    std::uint32_t expire_tick,
    std::uint16_t damage_per_interval,
    std::uint8_t source_code,
    std::uint32_t collision_mask) {
    const entt::entity entity =
        create_networked_entity(EntityType::kAreaEffect, owner_peer, position);
    registry().emplace<AreaEffectTag>(entity);
    registry().emplace<Hitbox>(
        entity,
        Hitbox{{0.0f, 0.0f, 0.0f}, {radius, radius, radius}, 0});
    registry().emplace<AreaEffectState>(
        entity,
        AreaEffectState{
            radius,
            damage_per_interval,
            damage_interval_ticks == 0 ? 1u : damage_interval_ticks,
            expire_tick,
            source_code,
            collision_mask,
            {},
        });
    return registry().get<NetworkIdentity>(entity).net_id;
}

NetId World::spawn_beam(
    PeerId owner_peer,
    NetId shooter_net_id,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float length,
    float radius,
    std::uint16_t damage_per_second,
    std::uint32_t expire_tick,
    std::uint8_t source_code,
    std::uint32_t collision_mask) {
    const entt::entity entity =
        create_networked_entity(EntityType::kBeam, owner_peer, origin);
    registry().emplace<BeamTag>(entity);
    registry().emplace<Hitbox>(
        entity,
        Hitbox{{0.0f, 0.0f, 0.0f}, {radius, radius, radius}, 0});
    registry().emplace<BeamState>(
        entity,
        BeamState{
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
    return registry().get<NetworkIdentity>(entity).net_id;
}

}  // namespace network_example
