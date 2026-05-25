#include "kernel/src/render_state_builder.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

namespace network_example {
namespace {

KernelVec3 to_kernel_vec3(const glm::vec3& value) {
    return KernelVec3{value.x, value.y, value.z};
}

KernelQuat to_kernel_quat(const glm::quat& value) {
    return KernelQuat{value.x, value.y, value.z, value.w};
}

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

RenderEntityState render_state_from_world_entity(
    const World& world,
    entt::entity entity,
    std::uint64_t entity_id) {
    const NetworkIdentity& identity = world.registry().get<NetworkIdentity>(entity);
    const EntityKind& kind = world.registry().get<EntityKind>(entity);
    const Transform& transform = world.registry().get<Transform>(entity);
    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    std::uint16_t animation_state = 0;
    std::uint32_t visual_flags = derived_visual_flags(world, entity);
    std::uint32_t spawn_tick = 0;
    std::uint32_t client_action_id = 0;
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
    if (world.registry().all_of<Velocity>(entity)) {
        velocity = to_kernel_vec3(world.registry().get<Velocity>(entity).linear);
    }
    if (world.registry().all_of<Health>(entity)) {
        const Health& health = world.registry().get<Health>(entity);
        hp = health.hp;
        max_hp = health.max_hp;
    }
    if (world.registry().all_of<ReplicationState>(entity)) {
        const ReplicationState& replication =
            world.registry().get<ReplicationState>(entity);
        animation_state = replication.animation_state;
        visual_flags |= replication.visual_flags;
    }
    if (world.registry().all_of<ProjectileState>(entity)) {
        const ProjectileState& projectile =
            world.registry().get<ProjectileState>(entity);
        spawn_tick = projectile.spawn_tick;
        client_action_id = projectile.client_action_id;
    }
    if (world.registry().all_of<HomingState>(entity)) {
        animation_state = static_cast<std::uint16_t>(
            world.registry().get<HomingState>(entity).phase);
    }
    return RenderEntityState{
        entity_id,
        identity.net_id,
        static_cast<std::uint16_t>(kind.type),
        identity.owner_peer,
        to_kernel_vec3(transform.position),
        to_kernel_quat(transform.rotation),
        velocity,
        hp,
        max_hp,
        animation_state,
        visual_flags,
        spawn_tick,
        client_action_id,
    };
}

RenderEntityState render_state_from_snapshot_entity(
    const EntitySnapshot& entity,
    std::uint64_t entity_id) {
    return RenderEntityState{
        entity_id,
        entity.net_id,
        static_cast<std::uint16_t>(entity.type),
        entity.owner_peer,
        to_kernel_vec3(entity.position),
        to_kernel_quat(entity.rotation),
        to_kernel_vec3(entity.velocity),
        entity.hp,
        entity.max_hp,
        entity.state,
        entity.flags,
        entity.spawn_tick,
        entity.client_action_id,
    };
}

EntitySnapshot interpolate_snapshot_entity(
    const EntitySnapshot& from,
    const EntitySnapshot& to,
    float alpha) {
    EntitySnapshot entity = to;
    entity.position = from.position + (to.position - from.position) * alpha;
    entity.rotation = glm::slerp(from.rotation, to.rotation, alpha);
    entity.velocity = from.velocity + (to.velocity - from.velocity) * alpha;
    return entity;
}

}  // namespace network_example
