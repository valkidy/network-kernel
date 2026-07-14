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
        if (world.registry().all_of<ServerOnly>(entity)) {
            continue;
        }
        EntitySnapshot entity_snapshot;
        const NetworkIdentity& identity = view.get<const NetworkIdentity>(entity);
        entity_snapshot.net_id = identity.net_id;
        entity_snapshot.owner_peer = identity.owner_peer;
        const EntityKind& kind = view.get<const EntityKind>(entity);
        entity_snapshot.type = kind.type;
        entity_snapshot.actor_type = kind.actor_type;
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
        entity_snapshot.flags &= ~kVisualFlagFiring;
        if (world.registry().all_of<ProjectileState>(entity)) {
            const ProjectileState& projectile =
                world.registry().get<ProjectileState>(entity);
            entity_snapshot.spawn_tick = projectile.spawn_tick;
            entity_snapshot.action_instance_id = projectile.action_instance_id;
        }
        if (world.registry().all_of<ActionInputState>(entity)) {
            entity_snapshot.aim_direction =
                world.registry().get<ActionInputState>(entity).aim_direction;
        }
        if (world.registry().all_of<ActionRuntimeState>(entity)) {
            const ActionRuntimeState& action =
                world.registry().get<ActionRuntimeState>(entity);
            entity_snapshot.action_template_id = action.action_template_id;
            entity_snapshot.action_instance_id = action.action_instance_id;
            entity_snapshot.action_start_tick = action.start_tick;
            entity_snapshot.action_commit_count = action.commit_count;
            entity_snapshot.action_phase = action.phase;
        }
        if (world.registry().all_of<HomingState>(entity)) {
            entity_snapshot.state = static_cast<std::uint16_t>(
                world.registry().get<HomingState>(entity).phase);
        }
        snapshot.entities.push_back(entity_snapshot);
    }

    return snapshot;
}

}  // namespace network_example
