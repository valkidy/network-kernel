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
    if (world.registry().all_of<MovementState>(entity)) {
        const MovementState& movement =
            world.registry().get<MovementState>(entity);
        flags |= movement.ground_state == MovementState::GroundState::kGrounded
            ? kVisualFlagGrounded
            : kVisualFlagFalling;
        if (movement.landed_this_tick) {
            flags |= kVisualFlagLanded;
        }
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
    std::uint32_t action_instance_id = 0;
    std::uint32_t template_id = 0;
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
    KernelActionRuntimeView action{};
    action.struct_size = sizeof(action);
    KernelVec3 aim_direction{1.0f, 0.0f, 0.0f};
    std::uint32_t item_template_id = 0;
    std::uint64_t item_instance_id = 0;
    std::uint8_t world_item_mode = 0;
    std::uint32_t carrier_entity_id = 0;
    if (world.registry().all_of<Velocity>(entity)) {
        velocity = to_kernel_vec3(world.registry().get<Velocity>(entity).linear);
    }
    if (world.registry().all_of<Health>(entity)) {
        const Health& health = world.registry().get<Health>(entity);
        hp = health.hp;
        max_hp = health.max_hp;
    } else {
        visual_flags |= kVisualFlagHpUnknown;
    }
    if (world.registry().all_of<ReplicationState>(entity)) {
        const ReplicationState& replication =
            world.registry().get<ReplicationState>(entity);
        animation_state = replication.animation_state;
        visual_flags |= replication.visual_flags;
    }
    visual_flags &= ~kVisualFlagFiring;
    if (world.registry().all_of<ProjectileState>(entity)) {
        const ProjectileState& projectile =
            world.registry().get<ProjectileState>(entity);
        spawn_tick = projectile.spawn_tick;
        action_instance_id = projectile.action_instance_id;
        template_id = projectile.projectile_template_id;
    }
    if (world.registry().all_of<ActorTemplateRef>(entity)) {
        template_id =
            world.registry().get<ActorTemplateRef>(entity).actor_template_id;
    }
    if (world.registry().all_of<ActionInputState>(entity)) {
        aim_direction = to_kernel_vec3(
            world.registry().get<ActionInputState>(entity).aim_direction);
    }
    if (world.registry().all_of<ActionRuntimeState>(entity)) {
        const ActionRuntimeState& runtime =
            world.registry().get<ActionRuntimeState>(entity);
        action.action_template_id = runtime.action_template_id;
        action.action_instance_id = runtime.action_instance_id;
        action.phase = runtime.phase;
        action.start_tick = runtime.start_tick;
        action.commit_count = runtime.commit_count;
        if (runtime.phase == KernelActionPhase_Active) {
            visual_flags |= kVisualFlagFiring;
        }
    }
    if (world.registry().all_of<HomingState>(entity)) {
        animation_state = static_cast<std::uint16_t>(
            world.registry().get<HomingState>(entity).phase);
    }
    if (world.registry().all_of<ItemTemplateRef>(entity)) {
        item_template_id =
            world.registry().get<ItemTemplateRef>(entity).item_template_id;
    }
    if (world.registry().all_of<ItemInstanceRef>(entity)) {
        item_instance_id =
            world.registry().get<ItemInstanceRef>(entity).item_instance_id;
    }
    if (world.registry().all_of<PropWorldMode>(entity)) {
        world_item_mode = static_cast<std::uint8_t>(
            world.registry().get<PropWorldMode>(entity).mode);
    }
    if (world.registry().all_of<CarriedBy>(entity)) {
        carrier_entity_id =
            world.registry().get<CarriedBy>(entity).carrier_entity_id;
    }
    if (kind.type == EntityType::kProp && item_instance_id != 0u &&
        item_template_id != 0u) {
        template_id = item_template_id;
    } else if (template_id == 0u &&
               world.registry().all_of<EntityTemplateRef>(entity)) {
        template_id =
            world.registry().get<EntityTemplateRef>(entity).entity_template_id;
    }
    return RenderEntityState{
        entity_id,
        identity.net_id,
        static_cast<std::uint16_t>(kind.type),
        static_cast<std::uint16_t>(kind.actor_type),
        identity.owner_peer,
        to_kernel_vec3(transform.position),
        to_kernel_quat(transform.rotation),
        velocity,
        hp,
        max_hp,
        animation_state,
        visual_flags,
        spawn_tick,
        action_instance_id,
        RenderEntityStatus_Active,
        template_id,
        0,
        action,
        aim_direction,
        item_instance_id,
        world_item_mode,
        0,
        0,
        carrier_entity_id,
    };
}

RenderEntityState render_state_from_snapshot_entity(
    const EntitySnapshot& entity,
    std::uint64_t entity_id) {
    return RenderEntityState{
        entity_id,
        entity.net_id,
        static_cast<std::uint16_t>(entity.type),
        static_cast<std::uint16_t>(entity.actor_type),
        entity.owner_peer,
        to_kernel_vec3(entity.position),
        to_kernel_quat(entity.rotation),
        to_kernel_vec3(entity.velocity),
        entity.hp,
        entity.max_hp,
        entity.state,
        (entity.flags & ~kVisualFlagFiring) |
            (entity.action_phase == KernelActionPhase_Active
                 ? kVisualFlagFiring
                 : 0u) |
            ((entity.state_flags & kSnapshotStateFlagHpUnknown) != 0u
                 ? kVisualFlagHpUnknown
                 : 0u),
        entity.spawn_tick,
        entity.action_instance_id,
        RenderEntityStatus_Active,
        0,
        0,
        KernelActionRuntimeView{
            sizeof(KernelActionRuntimeView),
            entity.action_template_id,
            entity.action_instance_id,
            entity.action_phase,
            0,
            0,
            entity.action_start_tick,
            entity.action_commit_count,
        },
        to_kernel_vec3(entity.aim_direction),
        entity.item_instance_id,
        entity.world_item_mode,
        0,
        0,
        entity.carrier_entity_id,
        to_kernel_vec3(entity.beam_end),
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
    // A beam sweeps as its owner turns and its reach jumps whenever what blocks
    // it changes, so the far end has to travel with the near one. Left alone it
    // would hold the newer snapshot's endpoint against an interpolated origin
    // and the beam would visibly stretch and snap between snapshots.
    entity.beam_end = from.beam_end + (to.beam_end - from.beam_end) * alpha;
    return entity;
}

}  // namespace network_example
