#include "simulation/src/systems.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <variant>
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

KernelVec3 to_kernel_vec3(const glm::vec3& value) {
    return KernelVec3{value.x, value.y, value.z};
}

bool same_vec3(const KernelVec3& lhs, const KernelVec3& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
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

const KernelEntityTemplateDefinition* find_entity_template(
    const std::vector<KernelEntityTemplateDefinition>& templates,
    std::uint32_t entity_template_id) {
    const auto found = std::find_if(
        templates.begin(),
        templates.end(),
        [entity_template_id](const KernelEntityTemplateDefinition& entity_template) {
            return entity_template.entity_template_id == entity_template_id;
        });
    return found == templates.end() ? nullptr : &*found;
}

AiControllerType to_ai_controller_type(std::uint32_t controller_type) {
    if (controller_type == KernelAiControllerType_Sentry) {
        return AiControllerType::kSentry;
    }
    if (controller_type == KernelAiControllerType_Director) {
        return AiControllerType::kDirector;
    }
    return AiControllerType::kNone;
}

std::uint32_t live_agent_count(World& world) {
    std::uint32_t count = 0;
    auto actor_view = world.registry().view<const EntityKind>();
    for (const entt::entity entity : actor_view) {
        const EntityKind& kind = actor_view.get<const EntityKind>(entity);
        if (kind.type == EntityType::kActor && kind.actor_type == ActorType::kAgent) {
            ++count;
        }
    }
    return count;
}

const ai::AIValue* intent_param(
    const ai::ScopedIntent& intent,
    const char* key) {
    const auto found = intent.params.find(key);
    return found == intent.params.end() ? nullptr : &found->second;
}

std::optional<std::uint32_t> uint32_param(
    const ai::ScopedIntent& intent,
    const char* key) {
    const ai::AIValue* value = intent_param(intent, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto* typed = std::get_if<std::uint32_t>(value)) {
        return *typed;
    }
    if (const auto* typed = std::get_if<int>(value); typed != nullptr && *typed >= 0) {
        return static_cast<std::uint32_t>(*typed);
    }
    return std::nullopt;
}

std::optional<float> float_param(
    const ai::ScopedIntent& intent,
    const char* key) {
    const ai::AIValue* value = intent_param(intent, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto* typed = std::get_if<float>(value)) {
        return *typed;
    }
    if (const auto* typed = std::get_if<std::uint32_t>(value)) {
        return static_cast<float>(*typed);
    }
    if (const auto* typed = std::get_if<int>(value)) {
        return static_cast<float>(*typed);
    }
    return std::nullopt;
}

void materialize_ai_runtime(
    entt::registry& registry,
    entt::entity entity,
    const KernelEntityAiDefinition& ai) {
    if (ai.controller_type == KernelAiControllerType_None) {
        return;
    }
    registry.emplace_or_replace<AgentRuntime>(
        entity,
        AgentRuntime{
            ai.ai_profile_id,
            to_ai_controller_type(ai.controller_type),
            ai.tick_interval == 0u ? 1u : ai.tick_interval,
            0u,
            ai.blackboard_id,
        });
}

void materialize_director_runtime(
    entt::registry& registry,
    entt::entity entity,
    const KernelEntityAiDefinition& ai) {
    if (ai.controller_type != KernelAiControllerType_Director) {
        return;
    }
    registry.emplace_or_replace<DirectorRuntime>(
        entity,
        DirectorRuntime{
            ai.tick_interval == 0u ? 1u : ai.tick_interval,
            0u,
            ai.spawn_target_count,
            ai.spawn_entity_template_id,
            ai.spawn_actor_template_id,
            from_kernel_vec3(ai.spawn_position),
            ai.spawn_radius,
            ai.spawn_seed == 0u ? 1u : ai.spawn_seed,
            0u,
        });
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

    EntityType type = static_cast<EntityType>(create_info.entity_type);
    ActorType actor_type = static_cast<ActorType>(create_info.actor_type);
    NetId net_id = 0;
    const KernelEntityTemplateDefinition* entity_template = nullptr;
    if (create_info.entity_template_id != 0u) {
        entity_template =
            find_entity_template(engine.entity_templates_, create_info.entity_template_id);
        if (entity_template == nullptr) {
            return false;
        }
        type = static_cast<EntityType>(entity_template->entity_type);
        actor_type = static_cast<ActorType>(entity_template->actor_type);
        net_id = engine.world_.spawn_entity(
            type,
            actor_type,
            create_info.owner_peer,
            from_kernel_vec3(create_info.position));
    } else if (type == EntityType::kActor && actor_type == ActorType::kPlayer) {
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
    if (entity_template != nullptr) {
        entt::registry& registry = engine.world_.registry();
        if (type == EntityType::kActor && actor_type == ActorType::kPlayer) {
            registry.emplace_or_replace<PlayerTag>(*entity);
        } else if (type == EntityType::kActor && actor_type == ActorType::kAgent) {
            registry.emplace_or_replace<AgentTag>(*entity);
        }
        if ((entity_template->component_flags & KERNEL_ENTITY_COMPONENT_SERVER_ONLY) !=
            0u) {
            registry.emplace_or_replace<ServerOnly>(*entity);
        }
        if ((entity_template->component_flags & KERNEL_ENTITY_COMPONENT_VELOCITY) !=
            0u) {
            registry.get_or_emplace<Velocity>(*entity);
        }
        if ((entity_template->component_flags & KERNEL_ENTITY_COMPONENT_HEALTH) !=
            0u) {
            registry.emplace_or_replace<Health>(
                *entity,
                Health{
                    entity_template->combat.hp,
                    entity_template->combat.max_hp,
                });
            MovementState& movement = registry.get_or_emplace<MovementState>(*entity);
            movement.speed_meters_per_second =
                entity_template->combat.move_speed_meters_per_second;
            movement.controller_type = static_cast<MovementState::ControllerType>(
                entity_template->movement.controller_type);
            movement.movement_collider_template_id =
                entity_template->movement.movement_collider_template_id;
            movement.gravity = from_kernel_vec3(entity_template->movement.gravity);
            movement.max_slope_degrees =
                entity_template->movement.max_slope_degrees;
            movement.step_height = entity_template->movement.step_height;
            movement.ground_probe_distance =
                entity_template->movement.ground_probe_distance;
            movement.ground_snap_distance =
                entity_template->movement.ground_snap_distance;
        }
        if ((entity_template->component_flags &
            KERNEL_ENTITY_COMPONENT_WEAPON_STATE) != 0u) {
            WeaponState& weapon = registry.get_or_emplace<WeaponState>(*entity);
            weapon.active_weapon_slot =
                entity_template->combat.active_weapon_slot;
            weapon.weapon_slot_count =
                entity_template->combat.weapon_slot_count;
            for (std::size_t slot = 0; slot < kWeaponSlotCount; ++slot) {
                weapon.weapon_ids[slot] =
                    entity_template->combat.weapon_ids[slot];
                weapon.ammo[slot] = entity_template->combat.ammo[slot];
                weapon.reserve_magazines[slot] =
                    entity_template->combat.reserve_magazines[slot];
            }
            registry.get_or_emplace<WeaponTuning>(*entity);
        }
        if ((entity_template->component_flags & KERNEL_ENTITY_COMPONENT_HITBOX) !=
            0u) {
            registry.emplace_or_replace<Hitbox>(
                *entity,
                Hitbox{
                    from_kernel_vec3(entity_template->combat.hitbox_center),
                    from_kernel_vec3(entity_template->combat.hitbox_half_extents),
                    entity_template->collider_template_id,
                });
        }
        if ((entity_template->component_flags &
             KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME) != 0u) {
            materialize_ai_runtime(registry, *entity, entity_template->ai);
        }
        if ((entity_template->component_flags &
             KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME) != 0u) {
            registry.get_or_emplace<AgentSentryRuntime>(*entity);
        }
        if ((entity_template->component_flags &
             KERNEL_ENTITY_COMPONENT_DIRECTOR_RUNTIME) != 0u) {
            materialize_director_runtime(registry, *entity, entity_template->ai);
        }
        if (entity_template->actor_template_id != 0u) {
            registry.emplace_or_replace<ActorTemplateRef>(
                *entity,
                entity_template->actor_template_id);
        }
        if (entity_template->vision.struct_size >=
            sizeof(KernelAgentVisionConfig) &&
            !engine.server_set_entity_vision_config(net_id, entity_template->vision)) {
            return false;
        }
    }
    if (entity_template == nullptr &&
        type == EntityType::kActor &&
        actor_type == ActorType::kAgent) {
        entt::registry& registry = engine.world_.registry();
        registry.emplace_or_replace<AgentRuntime>(
            *entity,
            AgentRuntime{
                0u,
                AiControllerType::kSentry,
                1u,
                0u,
                0u,
            });
        registry.get_or_emplace<AgentSentryRuntime>(*entity);
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
    if (engine.physics_world_ != nullptr) {
        engine.physics_world_->remove_character(net_id);
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
    const KernelEntityTemplateDefinition* authored_entity_template = nullptr;
    for (const KernelEntityTemplateDefinition& candidate :
         engine.entity_templates_) {
        if (candidate.entity_type == KernelEntityType_Actor &&
            candidate.actor_template_id == actor_template_id) {
            authored_entity_template = &candidate;
            break;
        }
    }
    if (authored_entity_template != nullptr) {
        MovementState& movement =
            engine.world_.registry().get_or_emplace<MovementState>(*entity);
        movement.controller_type = static_cast<MovementState::ControllerType>(
            authored_entity_template->movement.controller_type);
        movement.movement_collider_template_id =
            authored_entity_template->movement.movement_collider_template_id;
        movement.gravity =
            from_kernel_vec3(authored_entity_template->movement.gravity);
        movement.max_slope_degrees =
            authored_entity_template->movement.max_slope_degrees;
        movement.step_height = authored_entity_template->movement.step_height;
        movement.ground_probe_distance =
            authored_entity_template->movement.ground_probe_distance;
        movement.ground_snap_distance =
            authored_entity_template->movement.ground_snap_distance;
        movement.ground_state = MovementState::GroundState::kAirborne;
        movement.has_last_queried_position = false;
        movement.landed_this_tick = false;
        if (engine.physics_world_ != nullptr) {
            engine.physics_world_->remove_character(net_id);
        }
    }
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

DirectorIntentExecutionResult DirectorIntentExecutor::execute(
    KernelEngine& engine,
    const ai::ScopedIntent& intent) const {
    DirectorIntentExecutionResult result;
    if (intent.scope != ai::IntentScope::kDirector ||
        intent.type != "SpawnAgent") {
        result.status = ai::IntentStatus::kFailed;
        result.unsupported = true;
        return result;
    }
    const std::optional<std::uint32_t> spawn_count =
        uint32_param(intent, "count");
    const std::optional<std::uint32_t> spawn_target_count =
        uint32_param(intent, "spawn_target_count");
    const std::optional<std::uint32_t> spawn_entity_template_id =
        uint32_param(intent, "spawn_entity_template_id");
    const std::optional<std::uint32_t> spawn_actor_template_id =
        uint32_param(intent, "spawn_actor_template_id");
    const std::optional<float> spawn_position_x =
        float_param(intent, "spawn_position_x");
    const std::optional<float> spawn_position_y =
        float_param(intent, "spawn_position_y");
    const std::optional<float> spawn_position_z =
        float_param(intent, "spawn_position_z");
    const std::optional<float> spawn_radius =
        float_param(intent, "spawn_radius");
    const std::optional<std::uint32_t> base_spawn_cursor =
        uint32_param(intent, "base_spawn_cursor");
    if (!spawn_count.has_value() || !spawn_target_count.has_value() ||
        !spawn_entity_template_id.has_value() ||
        !spawn_actor_template_id.has_value() || !spawn_position_x.has_value() ||
        !spawn_position_y.has_value() || !spawn_position_z.has_value() ||
        !spawn_radius.has_value() || !base_spawn_cursor.has_value()) {
        return result;
    }
    const KernelVec3 spawn_position{
        *spawn_position_x,
        *spawn_position_y,
        *spawn_position_z,
    };
    if (!engine.running_ || !is_server_mode(engine.config_.mode) ||
        intent.subject == 0 || *spawn_count == 0) {
        return result;
    }

    const std::optional<entt::entity> entity =
        engine.world_.find_entity(intent.subject);
    if (!entity.has_value() ||
        !engine.world_.registry().all_of<
            AgentRuntime,
            DirectorRuntime,
            Transform,
            ServerOnly,
            EntityKind>(*entity)) {
        return result;
    }

    auto& registry = engine.world_.registry();
    const EntityKind& kind = registry.get<EntityKind>(*entity);
    const AgentRuntime& agent = registry.get<AgentRuntime>(*entity);
    DirectorRuntime& director = registry.get<DirectorRuntime>(*entity);
    if (kind.type != EntityType::kDirector ||
        agent.controller_type != AiControllerType::kDirector ||
        *spawn_target_count != director.spawn_target_count ||
        *spawn_entity_template_id != director.spawn_entity_template_id ||
        *spawn_actor_template_id != director.spawn_actor_template_id ||
        !same_vec3(spawn_position, to_kernel_vec3(director.spawn_position)) ||
        *spawn_radius != director.spawn_radius ||
        *base_spawn_cursor != director.spawn_cursor) {
        return result;
    }

    if (*spawn_entity_template_id != 0u) {
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(
                engine.entity_templates_,
                *spawn_entity_template_id);
        if (entity_template == nullptr ||
            entity_template->entity_type != KernelEntityType_Actor ||
            entity_template->actor_type != KernelActorType_Agent) {
            return result;
        }
    } else {
        const KernelActorTemplateDefinition* actor_template =
            find_actor_template(
                engine.actor_templates_,
                *spawn_actor_template_id);
        if (actor_template == nullptr ||
            actor_template->entity_type != KernelEntityType_Actor ||
            actor_template->actor_type != KernelActorType_Agent) {
            return result;
        }
    }

    const std::uint32_t live_count = live_agent_count(engine.world_);
    if (*spawn_target_count <= live_count) {
        return result;
    }
    const std::uint32_t allowed_count =
        std::min(*spawn_count, *spawn_target_count - live_count);
    for (std::uint32_t index = 0; index < allowed_count; ++index) {
        const float angle =
            static_cast<float>(*base_spawn_cursor + index) * 2.39996323f;
        const float radius = *spawn_radius;
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.owner_peer = 0;
        create_info.position = KernelVec3{
            spawn_position.x + std::cos(angle) * radius,
            spawn_position.y,
            spawn_position.z + std::sin(angle) * radius,
        };
        create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        if (*spawn_entity_template_id != 0u) {
            create_info.entity_template_id = *spawn_entity_template_id;
        } else {
            create_info.entity_type = static_cast<std::uint16_t>(EntityType::kActor);
            create_info.actor_type = static_cast<std::uint16_t>(ActorType::kAgent);
            create_info.actor_template_id = *spawn_actor_template_id;
        }

        simulation::Command command{};
        command.id = simulation::CommandId::kCreateEntity;
        command.source = simulation::CommandSource::kAi;
        command.create_entity.create_info = create_info;
        if (!engine.enqueue_simulation_command(command)) {
            if (result.created_count > 0) {
                director.spawn_cursor += result.created_count;
                director.next_tick =
                    engine.tick_loop_.current_tick() +
                    std::max<std::uint32_t>(1u, director.tick_interval);
            }
            result.status = ai::IntentStatus::kFailed;
            return result;
        }
        ++result.created_count;
    }
    director.spawn_cursor += result.created_count;
    director.next_tick =
        engine.tick_loop_.current_tick() +
        std::max<std::uint32_t>(1u, director.tick_interval);
    result.status = ai::IntentStatus::kSucceeded;
    return result;
}

void DirectorIntentExecutor::update(KernelEngine& engine) const {
    std::vector<ai::ScopedIntent> intents =
        std::move(engine.pending_director_intents_);
    engine.pending_director_intents_.clear();
    engine.last_director_intent_processed_count_ = intents.size();
    engine.last_director_intent_created_count_ = 0;
    engine.last_director_intent_failed_count_ = 0;
    engine.last_director_intent_unsupported_count_ = 0;

    for (const ai::ScopedIntent& intent : intents) {
        const DirectorIntentExecutionResult result = execute(engine, intent);
        engine.last_director_intent_created_count_ += result.created_count;
        if (result.unsupported) {
            ++engine.last_director_intent_unsupported_count_;
        } else if (result.status == ai::IntentStatus::kFailed) {
            ++engine.last_director_intent_failed_count_;
        }
    }
}

void DirectorAISystem::update(KernelEngine& engine) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode)) {
        return;
    }

    std::uint32_t live_count = live_agent_count(engine.world_);

    auto director_view =
        engine.world_.registry()
            .view<
                EntityKind,
                NetworkIdentity,
                AgentRuntime,
                DirectorRuntime,
                Transform,
                ServerOnly>();
    for (const entt::entity entity : director_view) {
        const EntityKind& kind = director_view.get<EntityKind>(entity);
        const NetworkIdentity& identity = director_view.get<NetworkIdentity>(entity);
        AgentRuntime& agent = director_view.get<AgentRuntime>(entity);
        DirectorRuntime& director = director_view.get<DirectorRuntime>(entity);
        if (kind.type != EntityType::kDirector ||
            agent.controller_type != AiControllerType::kDirector ||
            director.spawn_target_count <= live_count ||
            engine.tick_loop_.current_tick() < director.next_tick) {
            continue;
        }

        const std::uint32_t missing_count =
            director.spawn_target_count - live_count;
        ai::ScopedIntent intent;
        intent.scope = ai::IntentScope::kDirector;
        intent.type = "SpawnAgent";
        intent.subject = identity.net_id;
        intent.params["count"] = missing_count;
        intent.params["spawn_target_count"] = director.spawn_target_count;
        intent.params["spawn_entity_template_id"] =
            director.spawn_entity_template_id;
        intent.params["spawn_actor_template_id"] =
            director.spawn_actor_template_id;
        intent.params["spawn_position_x"] = director.spawn_position.x;
        intent.params["spawn_position_y"] = director.spawn_position.y;
        intent.params["spawn_position_z"] = director.spawn_position.z;
        intent.params["spawn_radius"] = director.spawn_radius;
        intent.params["base_spawn_cursor"] = director.spawn_cursor;
        engine.pending_director_intents_.push_back(std::move(intent));
        live_count += missing_count;
    }
}

}  // namespace network_example
