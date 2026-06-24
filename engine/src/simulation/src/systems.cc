#include "simulation/src/systems.h"

#include <algorithm>
#include <optional>
#include <vector>

#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>

#include "kernel/src/kernel.h"

namespace network_example {
namespace {

constexpr PeerId kLocalListenPeerId = 1;

bool is_server_mode(KernelMode mode) {
    return mode == KernelMode_DedicatedServer || mode == KernelMode_ListenServer;
}

glm::vec3 from_kernel_vec3(const KernelVec3& value) {
    return glm::vec3{value.x, value.y, value.z};
}

glm::quat from_kernel_quat(const KernelQuat& value) {
    return glm::quat{value.w, value.x, value.y, value.z};
}

const KernelActorTemplateDefinition* find_actor_template(
    const std::vector<KernelActorTemplateDefinition>& templates,
    std::uint32_t actor_template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [actor_template_id](const KernelActorTemplateDefinition& actor_template) {
            return actor_template.actor_template_id == actor_template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

KernelEntityLifecycleEventType lifecycle_type_for_despawn_reason(
    std::uint32_t reason) {
    if (reason == KernelDespawnReason_OutOfRange) {
        return KernelEntityLifecycleEventType_OutOfRange;
    }
    if (reason == KernelDespawnReason_Destroyed) {
        return KernelEntityLifecycleEventType_Destroyed;
    }
    return KernelEntityLifecycleEventType_Despawned;
}

}  // namespace

bool EntityLifecycleSystem::create_entity(
    KernelEngine& engine,
    const KernelServerEntityCreateInfo& create_info,
    NetId* out_net_id) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) ||
        out_net_id == nullptr ||
        create_info.struct_size < sizeof(KernelServerEntityCreateInfo)) {
        return false;
    }

    const EntityType type = static_cast<EntityType>(create_info.entity_type);
    const ActorType actor_type = static_cast<ActorType>(create_info.actor_type);
    NetId net_id = 0;
    if (type == EntityType::kActor && actor_type == ActorType::kPlayer) {
        net_id = engine.world_.spawn_player(
            create_info.owner_peer,
            from_kernel_vec3(create_info.position));
    } else if (type == EntityType::kActor && actor_type == ActorType::kAgent) {
        net_id = engine.world_.spawn_enemy(from_kernel_vec3(create_info.position));
    } else {
        return false;
    }

    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    if (create_info.actor_template_id != 0u) {
        if (find_actor_template(engine.actor_templates_, create_info.actor_template_id) ==
            nullptr) {
            return false;
        }
        engine.world_.registry().emplace_or_replace<ActorTemplateRef>(
            *entity,
            create_info.actor_template_id);
    }
    Transform& transform = engine.world_.registry().get<Transform>(*entity);
    transform.rotation = from_kernel_quat(create_info.rotation);
    ReplicationState& replication =
        engine.world_.registry().get_or_emplace<ReplicationState>(*entity);
    replication.animation_state = create_info.animation_state;
    replication.visual_flags = create_info.visual_flags;
    engine.materialize_entity_collider(net_id);

    *out_net_id = net_id;
    engine.push_event(
        KernelEventType_EntitySpawned,
        net_id,
        create_info.owner_peer,
        static_cast<std::uint32_t>(type));
    engine.publish_snapshot();
    return true;
}

bool EntityLifecycleSystem::destroy_entity(
    KernelEngine& engine,
    NetId net_id,
    std::uint32_t reason) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) || net_id == 0) {
        return false;
    }
    std::uint16_t entity_type = 0;
    std::uint16_t actor_type = 0;
    if (const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
        entity.has_value() && engine.world_.registry().all_of<EntityKind>(*entity)) {
        const EntityKind& kind = engine.world_.registry().get<EntityKind>(*entity);
        entity_type = static_cast<std::uint16_t>(kind.type);
        actor_type = static_cast<std::uint16_t>(kind.actor_type);
    }
    if (!engine.world_.destroy(net_id)) {
        return false;
    }
    engine.vision_configs_.erase(net_id);
    engine.vision_states_.erase(net_id);
    if (engine.config_.mode == KernelMode_ListenServer &&
        engine.listen_server_transport_ != nullptr) {
        engine.send_entity_despawn(kLocalListenPeerId, net_id, reason);
    }
    for (KernelEngine::PeerSession& session : engine.peer_sessions_) {
        if (session.relevant_entities.erase(net_id) > 0) {
            engine.send_entity_despawn(session.peer, net_id, reason);
        }
    }
    engine.push_event(KernelEventType_EntityDestroyed, net_id, 0, reason);
    engine.lifecycle_events_.push_back(KernelEntityLifecycleEvent{
        lifecycle_type_for_despawn_reason(reason),
        engine.tick_loop_.current_tick(),
        net_id,
        reason,
        entity_type,
        actor_type,
        0,
    });
    engine.publish_snapshot();
    return true;
}

bool EntityStateSystem::set_actor_template(
    KernelEngine& engine,
    NetId net_id,
    std::uint32_t actor_template_id) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) || net_id == 0 ||
        actor_template_id == 0u ||
        find_actor_template(engine.actor_templates_, actor_template_id) == nullptr) {
        return false;
    }
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value() ||
        !engine.world_.registry().all_of<EntityKind>(*entity) ||
        engine.world_.registry().get<EntityKind>(*entity).type != EntityType::kActor) {
        return false;
    }
    engine.world_.registry().emplace_or_replace<ActorTemplateRef>(
        *entity,
        actor_template_id);
    engine.materialize_entity_collider(net_id);
    if (engine.config_.mode == KernelMode_ListenServer &&
        engine.listen_server_transport_ != nullptr &&
        engine.local_listen_session_.relevant_entities.find(net_id) !=
            engine.local_listen_session_.relevant_entities.end()) {
        engine.send_entity_template_update(
            kLocalListenPeerId,
            net_id,
            actor_template_id);
    }
    if (is_server_mode(engine.config_.mode)) {
        for (const KernelEngine::PeerSession& session : engine.peer_sessions_) {
            if (session.welcomed &&
                session.relevant_entities.find(net_id) !=
                    session.relevant_entities.end()) {
                engine.send_entity_template_update(
                    session.peer,
                    net_id,
                    actor_template_id);
            }
        }
    }
    engine.rebuild_render_states();
    return true;
}

bool EntityStateSystem::set_transform(
    KernelEngine& engine,
    NetId net_id,
    const KernelVec3& position,
    const KernelQuat& rotation) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value() ||
        !engine.world_.registry().all_of<Transform>(*entity)) {
        return false;
    }
    Transform& transform = engine.world_.registry().get<Transform>(*entity);
    transform.position = from_kernel_vec3(position);
    transform.rotation = from_kernel_quat(rotation);
    engine.sync_entity_colliders_from_world();
    return true;
}

bool EntityStateSystem::set_velocity(
    KernelEngine& engine,
    NetId net_id,
    const KernelVec3& velocity) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value() ||
        !engine.world_.registry().all_of<Velocity>(*entity)) {
        return false;
    }
    engine.world_.registry().get<Velocity>(*entity).linear =
        from_kernel_vec3(velocity);
    return true;
}

bool EntityStateSystem::set_state(
    KernelEngine& engine,
    NetId net_id,
    std::uint16_t animation_state,
    std::uint32_t visual_flags) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode)) {
        return false;
    }
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    ReplicationState& replication =
        engine.world_.registry().get_or_emplace<ReplicationState>(*entity);
    replication.animation_state = animation_state;
    replication.visual_flags = visual_flags;
    return true;
}

bool MovementSystem::submit_input(
    KernelEngine& engine,
    NetId net_id,
    const PlayerInput& input) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) || net_id == 0) {
        return false;
    }
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value() ||
        !engine.world_.registry().all_of<NetworkIdentity, Transform, WeaponState, Hitbox>(
            *entity)) {
        return false;
    }

    engine.pending_inputs_.push_back(QueuedInput{
        0,
        input,
        engine.tick_loop_.current_tick(),
        engine.current_server_time_us(),
        true,
        net_id,
    });
    return true;
}

}  // namespace network_example
