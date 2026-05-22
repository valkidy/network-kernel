#include "sync/public/snapshot.h"

namespace network_example {
namespace {

std::uint32_t derived_visual_flags(const World& world, entt::entity entity) {
    std::uint32_t flags = 0;
    if (world.registry().all_of<Velocity>(entity) &&
        glm::length(world.registry().get<Velocity>(entity).linear) > 0.001f) {
        flags |= kVisualFlagMoving;
    }
    if (world.registry().all_of<WeaponState>(entity) &&
        world.registry().get<WeaponState>(entity).is_reloading) {
        flags |= kVisualFlagReloading;
    }
    if (world.registry().all_of<Health>(entity) &&
        world.registry().get<Health>(entity).hp == 0) {
        flags |= kVisualFlagDead;
    }
    return flags;
}

}  // namespace

WorldSnapshot build_world_snapshot(
    const World& world,
    std::uint32_t server_tick,
    std::uint32_t server_time_ms,
    std::uint32_t last_processed_input_seq) {
    WorldSnapshot snapshot;
    snapshot.header = SnapshotHeader{
        server_tick,
        server_time_ms,
        last_processed_input_seq,
    };

    auto view = world.registry().view<
        const NetworkIdentity,
        const EntityKind,
        const Transform>();
    for (const entt::entity entity : view) {
        EntitySnapshot entity_snapshot;
        const NetworkIdentity& identity = view.get<const NetworkIdentity>(entity);
        entity_snapshot.net_id = identity.net_id;
        entity_snapshot.owner_peer = identity.owner_peer;
        entity_snapshot.type = view.get<const EntityKind>(entity).type;
        entity_snapshot.position = view.get<const Transform>(entity).position;
        entity_snapshot.rotation = view.get<const Transform>(entity).rotation;
        if (world.registry().all_of<Velocity>(entity)) {
            entity_snapshot.velocity = world.registry().get<Velocity>(entity).linear;
        }
        if (world.registry().all_of<Health>(entity)) {
            const Health& health = world.registry().get<Health>(entity);
            entity_snapshot.hp = health.hp;
            entity_snapshot.max_hp = health.max_hp;
        }
        entity_snapshot.flags = derived_visual_flags(world, entity);
        if (world.registry().all_of<ReplicationState>(entity)) {
            const ReplicationState& replication =
                world.registry().get<ReplicationState>(entity);
            entity_snapshot.state = replication.animation_state;
            entity_snapshot.flags |= replication.visual_flags;
        }
        if (world.registry().all_of<ProjectileState>(entity)) {
            const ProjectileState& projectile =
                world.registry().get<ProjectileState>(entity);
            entity_snapshot.spawn_tick = projectile.spawn_tick;
            entity_snapshot.client_action_id = projectile.client_action_id;
        }
        snapshot.entities.push_back(entity_snapshot);
    }

    return snapshot;
}

}  // namespace network_example
