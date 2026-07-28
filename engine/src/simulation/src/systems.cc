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
#include "simulation/public/action_graph.h"

namespace network_example {
namespace {

constexpr PeerId kLocalListenPeerId = 1;

bool is_server_mode(KernelMode mode) {
    return mode == KernelMode_DedicatedServer || mode == KernelMode_ListenServer;
}

std::uint64_t collision_pair_key(NetId subject, NetId target) {
    return (static_cast<std::uint64_t>(subject) << 32u) |
        static_cast<std::uint64_t>(target);
}

std::uint64_t action_trigger_request_id(
    std::uint32_t server_tick,
    TriggerEventType event_type,
    NetId subject,
    NetId related_entity,
    std::uint32_t sequence) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint32_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(server_tick);
    mix(subject);
    mix(related_entity);
    mix(sequence);
    mix(static_cast<std::uint32_t>(event_type));
    return hash;
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

std::optional<CompiledActionGraphBinding> compile_entity_trigger_binding(
    const KernelActionTriggerDefinition& trigger,
    TriggerEventType event_type) {
    return compile_action_trigger_definition(event_type, trigger);
}

bool execute_action_graph_commands(
    KernelEngine& engine,
    World& world,
    DamagePipeline* damage_pipeline,
    const std::vector<KernelEntityTemplateDefinition>& entity_templates,
    const ActionGraphCommandBatch& batch,
    std::uint64_t server_time_us) {
    if (damage_pipeline == nullptr || batch.provenance.request_id == 0u) {
        return false;
    }
    if (world.action_graph_batch_processed(
            batch.provenance.requester_peer != 0u
                ? batch.provenance.requester_peer
                : batch.provenance.owner_peer,
            batch.provenance.request_id,
            batch.event.type,
            batch.sequence)) {
        return true;
    }
    const std::vector<ActionGraphCommand>& commands = batch.commands;
    for (const ActionGraphCommand& command : commands) {
        if (const auto* damage =
                std::get_if<ActionApplyDamageCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(damage->target);
            if (damage->source == 0u || !target.has_value() ||
                !world.registry().all_of<NetworkIdentity, Health>(*target)) {
                return false;
            }
            continue;
        }
        if (const auto* projectile =
                std::get_if<ActionSpawnProjectileCommand>(&command)) {
            if (world.find_projectile_template(
                    projectile->projectile_template_id) == nullptr ||
                !std::isfinite(projectile->position.x) ||
                !std::isfinite(projectile->position.y) ||
                !std::isfinite(projectile->position.z) ||
                !std::isfinite(projectile->direction.x) ||
                !std::isfinite(projectile->direction.y) ||
                !std::isfinite(projectile->direction.z) ||
                glm::dot(projectile->direction, projectile->direction) == 0.0f) {
                return false;
            }
            continue;
        }
        const auto* spawn = std::get_if<ActionSpawnEntityCommand>(&command);
        const std::optional<entt::entity> owner = spawn == nullptr
            ? std::nullopt
            : world.find_entity(spawn->owner);
        if (spawn == nullptr ||
            find_entity_template(entity_templates, spawn->entity_template_id) ==
                nullptr ||
            !owner.has_value() ||
            !world.registry().all_of<NetworkIdentity>(*owner) ||
            !std::isfinite(spawn->position.x) ||
            !std::isfinite(spawn->position.y) ||
            !std::isfinite(spawn->position.z)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const ActionGraphCommand& command = commands[index];
        if (const auto* damage =
                std::get_if<ActionApplyDamageCommand>(&command)) {
            if (!damage_pipeline->submit_damage_request(DamageRequest{
                    damage->provenance.server_tick,
                    static_cast<std::uint32_t>(index),
                    damage->source,
                    damage->target,
                    damage->provenance.owner_peer,
                    0u,
                    damage->amount,
                    server_time_us,
                    batch.event.position,
                })) {
                return false;
            }
            continue;
        }
        if (const auto* projectile =
                std::get_if<ActionSpawnProjectileCommand>(&command)) {
            if (!spawn_action_graph_projectile(
                    world,
                    projectile->projectile_template_id,
                    projectile->provenance.owner_peer,
                    projectile->provenance.instigator,
                    projectile->provenance.action_instance_id,
                    projectile->position,
                    projectile->direction,
                    projectile->provenance.server_tick,
                    engine.fixed_delta_seconds())) {
                return false;
            }
            continue;
        }
        const ActionSpawnEntityCommand& spawn =
            std::get<ActionSpawnEntityCommand>(command);
        const entt::entity owner = *world.find_entity(spawn.owner);
        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.entity_template_id = spawn.entity_template_id;
        create_info.owner_peer =
            world.registry().get<NetworkIdentity>(owner).owner_peer;
        create_info.position = to_kernel_vec3(spawn.position);
        create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        NetId spawned_net_id = 0;
        if (!EntityLifecycleSystem{}.create_entity(
                engine, create_info, &spawned_net_id)) {
            return false;
        }
    }
    world.mark_action_graph_batch_processed(
        batch.provenance.requester_peer != 0u
            ? batch.provenance.requester_peer
            : batch.provenance.owner_peer,
        batch.provenance.request_id,
        batch.event.type,
        batch.sequence);
    return true;
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

bool execute_action_graph_command_batch(
    KernelEngine& engine,
    const ActionGraphCommandBatch& batch,
    std::uint64_t server_time_us) {
    return execute_action_graph_commands(
        engine,
        engine.simulation_world(),
        &engine.damage_pipeline(),
        engine.authored_entity_templates(),
        batch,
        server_time_us);
}

bool EntityLifecycleSystem::create_entity(
    KernelEngine& engine,
    const KernelServerEntityCreateInfo& create_info,
    NetId* out_net_id,
    bool publish_snapshot) const {
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
        registry.emplace_or_replace<EntityTemplateRef>(
            *entity,
            entity_template->entity_template_id);
        if (type == EntityType::kProp) {
            registry.emplace_or_replace<PropWorldMode>(
                *entity,
                PropWorldMode{PropMode::kPlaced});
        }
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
        if (const std::optional<CompiledActionGraphBinding> binding =
                compile_entity_trigger_binding(
                    entity_template->activated_trigger,
                    TriggerEventType::kActivated)) {
            registry.emplace_or_replace<OnActivatedTriggerTag>(*entity);
            registry.emplace_or_replace<ActionGraphActivatedBinding>(
                *entity,
                ActionGraphActivatedBinding{*binding});
        }
        if (const std::optional<CompiledActionGraphBinding> binding =
                compile_entity_trigger_binding(
                    entity_template->collision_trigger,
                    TriggerEventType::kCollision)) {
            registry.emplace_or_replace<OnCollisionTriggerTag>(*entity);
            registry.emplace_or_replace<ActionGraphCollisionBinding>(
                *entity,
                ActionGraphCollisionBinding{*binding});
        }
        if (const std::optional<CompiledActionGraphBinding> binding =
                compile_entity_trigger_binding(
                    entity_template->health_depleted_trigger,
                    TriggerEventType::kHealthDepleted)) {
            registry.emplace_or_replace<OnHealthDepletedTriggerTag>(*entity);
            registry.emplace_or_replace<ActionGraphHealthDepletedBinding>(
                *entity,
                ActionGraphHealthDepletedBinding{*binding});
        }
        if (const std::optional<CompiledActionGraphBinding> binding =
                compile_entity_trigger_binding(
                    entity_template->destroy_entity_trigger,
                    TriggerEventType::kDestroyEntity)) {
            registry.emplace_or_replace<OnDestroyEntityTriggerTag>(*entity);
            registry.emplace_or_replace<ActionGraphDestroyEntityBinding>(
                *entity,
                ActionGraphDestroyEntityBinding{*binding});
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
    if (type == EntityType::kActor) {
        engine.register_actor_for_first_physics(net_id);
    }

    *out_net_id = net_id;
    engine.push_event(
        KernelEventType_EntitySpawned,
        net_id,
        create_info.owner_peer,
        static_cast<std::uint32_t>(type));
    if (publish_snapshot) {
        engine.publish_snapshot();
    }
    return true;
}

bool ActivationSystem::activate_entity(
    KernelEngine& engine,
    const KernelServerEntityActivateInfo& activate_info) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) ||
        activate_info.struct_size < sizeof(KernelServerEntityActivateInfo) ||
        activate_info.request_id == 0u || activate_info.subject_net_id == 0u ||
        activate_info.instigator_net_id == 0u) {
        return false;
    }
    const std::optional<entt::entity> subject =
        engine.world_.find_entity(activate_info.subject_net_id);
    const std::optional<entt::entity> instigator =
        engine.world_.find_entity(activate_info.instigator_net_id);
    if (!subject.has_value() || !instigator.has_value() ||
        !engine.world_.registry().all_of<
            OnActivatedTriggerTag,
            ActionGraphActivatedBinding,
            NetworkIdentity,
            Transform>(*subject) ||
        !engine.world_.registry().all_of<
            NetworkIdentity,
            EntityKind,
            Transform>(*instigator) ||
        engine.world_.registry().get<EntityKind>(*instigator).type !=
            EntityType::kActor) {
        return false;
    }
    const NetworkIdentity& instigator_identity =
        engine.world_.registry().get<NetworkIdentity>(*instigator);
    if (engine.world_.action_graph_batch_processed(
            instigator_identity.owner_peer,
            activate_info.request_id,
            TriggerEventType::kActivated,
            0u)) {
        return true;
    }
    if (activate_info.target_net_id != 0u &&
        !engine.world_.find_entity(activate_info.target_net_id).has_value()) {
        return false;
    }

    const Transform& subject_transform =
        engine.world_.registry().get<Transform>(*subject);
    const Transform& instigator_transform =
        engine.world_.registry().get<Transform>(*instigator);
    const glm::vec3 offset =
        subject_transform.position - instigator_transform.position;
    const glm::vec3 direction = glm::length(offset) > 0.0001f
        ? glm::normalize(offset)
        : glm::vec3{1.0f, 0.0f, 0.0f};
    const TriggerEvent event{
        TriggerEventType::kActivated,
        activate_info.subject_net_id,
        activate_info.instigator_net_id,
        activate_info.target_net_id,
        subject_transform.position,
        direction,
        std::nullopt,
    };
    const NetworkIdentity& subject_identity =
        engine.world_.registry().get<NetworkIdentity>(*subject);
    const ActionExecutionProvenance provenance{
        activate_info.request_id,
        activate_info.action_instance_id,
        engine.tick_loop_.current_tick(),
        activate_info.instigator_net_id,
        subject_identity.owner_peer,
        0u,
        ActionAuthoritySource::kAuthoritativeSimulation,
        instigator_identity.owner_peer,
    };
    const ActionGraphActivatedBinding& activated =
        engine.world_.registry().get<ActionGraphActivatedBinding>(*subject);
    std::vector<ActionGraphQueuedTrigger> queued_triggers{
        ActionGraphQueuedTrigger{
            activated.binding,
            activate_info.subject_net_id,
            event,
            provenance,
            0u,
        },
    };
    std::vector<ActionGraphCommandBatch> command_batches;
    if (!dispatch_action_graph_triggers(
            &queued_triggers, &command_batches, nullptr) ||
        command_batches.size() != 1u) {
        return false;
    }

    if (!execute_action_graph_commands(
            engine,
            engine.world_,
            &engine.damage_pipeline_,
            engine.entity_templates_,
            command_batches.front(),
            engine.current_server_time_us())) {
        return false;
    }
    return true;
}

void CollisionTriggerSystem::update(
    KernelEngine& engine,
    std::uint64_t server_time_us) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) ||
        engine.physics_world_ == nullptr) {
        engine.active_prop_collision_pairs_.clear();
        return;
    }

    struct CollisionFact {
        NetId subject = 0;
        NetId target = 0;
        PeerId owner_peer = 0;
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f};
        CompiledActionGraphBinding binding;
    };
    std::vector<CollisionFact> entered_collisions;
    std::unordered_set<std::uint64_t> current_pairs;
    auto view = engine.world_.registry().view<
        NetworkIdentity,
        EntityKind,
        OnCollisionTriggerTag,
        ActionGraphCollisionBinding>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        const EntityKind& kind = view.get<EntityKind>(entity);
        if (kind.type != EntityType::kProp) {
            continue;
        }
        const ActionGraphCollisionBinding& collision_binding =
            view.get<ActionGraphCollisionBinding>(entity);
        std::unordered_set<NetId> seen_targets;
        for (const ColliderInstance& collider :
             engine.world_.collider_registry().instances()) {
            if (collider.entity_net_id != identity.net_id || !collider.enabled ||
                (collider.purpose_flags & KernelColliderPurpose_Hit) == 0u ||
                collider.shape_type == ColliderShapeType::kSegment ||
                collider.shape_type == ColliderShapeType::kCone) {
                continue;
            }
            physics::OverlapRequest request{};
            request.shape.type = collider.shape_type == ColliderShapeType::kSphere
                ? physics::CollisionShapeType::kSphere
                : collider.shape_type == ColliderShapeType::kCapsule
                    ? physics::CollisionShapeType::kCapsule
                    : physics::CollisionShapeType::kBox;
            request.shape.half_extents = collider.half_extents;
            request.shape.radius = collider.radius;
            request.shape.capsule_half_height = collider.capsule_half_height;
            request.position = collider.world_center;
            request.rotation = collider.world_rotation;
            request.filter.collision_mask = physics::collision_layer_bit(
                physics::CollisionLayer::kDamageable);
            request.filter.ignored_entity_net_id = identity.net_id;
            request.filter.object_kind_mask =
                1u << static_cast<std::uint8_t>(
                    physics::CollisionObjectKind::kActorHitbox);
            for (const physics::CollisionHit& hit :
                 engine.physics_world_->overlap_all(request)) {
                if (hit.identity.entity_net_id == 0u ||
                    !seen_targets.insert(hit.identity.entity_net_id).second) {
                    continue;
                }
                const std::uint64_t pair = collision_pair_key(
                    identity.net_id, hit.identity.entity_net_id);
                current_pairs.insert(pair);
                if (!engine.active_prop_collision_pairs_.contains(pair)) {
                    entered_collisions.push_back(CollisionFact{
                        identity.net_id,
                        hit.identity.entity_net_id,
                        identity.owner_peer,
                        hit.position,
                        hit.normal,
                        collision_binding.binding,
                    });
                }
            }
        }
    }
    std::sort(
        entered_collisions.begin(),
        entered_collisions.end(),
        [](const CollisionFact& lhs, const CollisionFact& rhs) {
            return lhs.subject != rhs.subject
                ? lhs.subject < rhs.subject
                : lhs.target < rhs.target;
        });
    engine.active_prop_collision_pairs_ = std::move(current_pairs);

    auto in_flight_view = engine.world_.registry().view<
        NetworkIdentity,
        PropWorldMode,
        Velocity>();
    for (const entt::entity entity : in_flight_view) {
        PropWorldMode& mode = in_flight_view.get<PropWorldMode>(entity);
        if (mode.mode != PropMode::kInFlight) continue;
        const NetworkIdentity& identity =
            in_flight_view.get<NetworkIdentity>(entity);
        bool collided = false;
        for (const ColliderInstance& collider :
             engine.world_.collider_registry().instances()) {
            if (collider.entity_net_id != identity.net_id || !collider.enabled ||
                collider.shape_type == ColliderShapeType::kSegment ||
                collider.shape_type == ColliderShapeType::kCone) {
                continue;
            }
            physics::OverlapRequest request{};
            request.shape.type = collider.shape_type == ColliderShapeType::kSphere
                ? physics::CollisionShapeType::kSphere
                : collider.shape_type == ColliderShapeType::kCapsule
                    ? physics::CollisionShapeType::kCapsule
                    : physics::CollisionShapeType::kBox;
            request.shape.half_extents = collider.half_extents;
            request.shape.radius = collider.radius;
            request.shape.capsule_half_height = collider.capsule_half_height;
            request.position = collider.world_center;
            request.rotation = collider.world_rotation;
            request.filter.ignored_entity_net_id = identity.net_id;
            request.filter.object_kind_mask =
                (1u << static_cast<std::uint32_t>(
                    physics::CollisionObjectKind::kTerrain)) |
                (1u << static_cast<std::uint32_t>(
                    physics::CollisionObjectKind::kStaticObstacle));
            if (!engine.physics_world_->overlap_all(request).empty()) {
                collided = true;
                break;
            }
        }
        if (!collided) continue;
        mode.mode = PropMode::kPlaced;
        in_flight_view.get<Velocity>(entity).linear = glm::vec3{0.0f};
        if (engine.world_.registry().all_of<ItemInstanceRef>(entity)) {
            const ItemInstanceRef& ref =
                engine.world_.registry().get<ItemInstanceRef>(entity);
            engine.item_store_.set_world_mode(
                ref.item_instance_id,
                KernelWorldItemMode_Placed);
        }
    }

    std::vector<ActionGraphQueuedTrigger> queued_triggers;
    queued_triggers.reserve(entered_collisions.size());
    for (const CollisionFact& collision : entered_collisions) {
        if (engine.next_action_graph_sequence_ == 0u) {
            engine.next_action_graph_sequence_ = 1u;
        }
        const std::uint32_t sequence = engine.next_action_graph_sequence_++;
        const TriggerEvent event{
            TriggerEventType::kCollision,
            collision.subject,
            0u,
            collision.target,
            collision.position,
            collision.direction,
            std::nullopt,
        };
        const ActionExecutionProvenance provenance{
            action_trigger_request_id(
                engine.tick_loop_.current_tick(),
                TriggerEventType::kCollision,
                collision.subject,
                collision.target,
                sequence),
            0u,
            engine.tick_loop_.current_tick(),
            0u,
            collision.owner_peer,
            0u,
            ActionAuthoritySource::kAuthoritativeSimulation,
        };
        queued_triggers.push_back(ActionGraphQueuedTrigger{
            collision.binding,
            collision.subject,
            event,
            provenance,
            sequence,
        });
    }

    std::vector<ActionGraphCommandBatch> command_batches;
    if (!dispatch_action_graph_triggers(
            &queued_triggers, &command_batches, nullptr)) {
        return;
    }
    for (const ActionGraphCommandBatch& batch : command_batches) {
        (void)execute_action_graph_commands(
            engine,
            engine.world_,
            &engine.damage_pipeline_,
            engine.entity_templates_,
            batch,
            server_time_us);
    }
}

bool EntityLifecycleSystem::destroy_entity(
    KernelEngine& engine,
    NetId net_id,
    std::uint32_t reason) const {
    return destroy_entity_with_context(
        engine, net_id, reason, 0u, 0u, nullptr);
}

void EntityLifecycleSystem::process_health_depleted(
    KernelEngine& engine,
    const std::vector<ConfirmedDamage>& health_depleted,
    std::uint64_t server_time_us) const {
    std::vector<ActionGraphQueuedTrigger> queued_triggers;
    queued_triggers.reserve(health_depleted.size());
    for (const ConfirmedDamage& damage : health_depleted) {
        const std::optional<entt::entity> subject =
            engine.world_.find_entity(damage.target_net_id);
        if (!subject.has_value() ||
            !engine.world_.registry().all_of<
                NetworkIdentity,
                OnHealthDepletedTriggerTag,
                ActionGraphHealthDepletedBinding>(*subject)) {
            continue;
        }
        const NetworkIdentity& identity =
            engine.world_.registry().get<NetworkIdentity>(*subject);
        const ActionGraphHealthDepletedBinding& binding =
            engine.world_.registry().get<ActionGraphHealthDepletedBinding>(*subject);
        const TriggerEvent event{
            TriggerEventType::kHealthDepleted,
            damage.target_net_id,
            damage.source_net_id,
            0u,
            damage.hit_position,
            glm::vec3{0.0f},
            std::nullopt,
        };
        const ActionExecutionProvenance provenance{
            action_trigger_request_id(
                engine.tick_loop_.current_tick(),
                TriggerEventType::kHealthDepleted,
                damage.target_net_id,
                damage.source_net_id,
                damage.sequence_id),
            0u,
            engine.tick_loop_.current_tick(),
            damage.source_net_id,
            identity.owner_peer,
            damage.source_code,
            ActionAuthoritySource::kAuthoritativeSimulation,
        };
        queued_triggers.push_back(ActionGraphQueuedTrigger{
            binding.binding,
            damage.target_net_id,
            event,
            provenance,
            damage.sequence_id,
        });
    }

    std::vector<ActionGraphCommandBatch> command_batches;
    if (!dispatch_action_graph_triggers(
            &queued_triggers, &command_batches, nullptr)) {
        return;
    }
    for (const ActionGraphCommandBatch& batch : command_batches) {
        (void)execute_action_graph_commands(
            engine,
            engine.world_,
            &engine.damage_pipeline_,
            engine.entity_templates_,
            batch,
            server_time_us);
    }
}

void EntityLifecycleSystem::destroy_dead_entities(
    KernelEngine& engine,
    const std::vector<ConfirmedDamage>& health_depleted) const {
    std::vector<NetId> dead_entities;
    auto view = engine.world_.registry().view<NetworkIdentity, Health>();
    for (const entt::entity entity : view) {
        const Health& health = view.get<Health>(entity);
        if (health.max_hp > 0u && health.hp == 0u &&
            !engine.world_.registry().all_of<PlayerTag>(entity)) {
            dead_entities.push_back(view.get<NetworkIdentity>(entity).net_id);
        }
    }
    std::sort(dead_entities.begin(), dead_entities.end());
    for (const NetId net_id : dead_entities) {
        const auto cause = std::find_if(
            health_depleted.begin(),
            health_depleted.end(),
            [net_id](const ConfirmedDamage& damage) {
                return damage.target_net_id == net_id;
            });
        const NetId instigator = cause == health_depleted.end()
            ? 0u
            : cause->source_net_id;
        const std::uint8_t source_code = cause == health_depleted.end()
            ? 0u
            : cause->source_code;
        const glm::vec3* position = cause == health_depleted.end()
            ? nullptr
            : &cause->hit_position;
        (void)destroy_entity_with_context(
            engine,
            net_id,
            KernelDespawnReason_Destroyed,
            instigator,
            source_code,
            position);
    }
}

bool EntityLifecycleSystem::destroy_entity_with_context(
    KernelEngine& engine,
    NetId net_id,
    std::uint32_t reason,
    NetId instigator,
    std::uint8_t source_code,
    const glm::vec3* event_position) const {
    if (!engine.running_ || !is_server_mode(engine.config_.mode) || net_id == 0) {
        return false;
    }
    std::uint16_t entity_type = 0;
    std::uint16_t actor_type = 0;
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    if (!entity.has_value()) {
        return false;
    }
    glm::vec3 position = event_position == nullptr
        ? glm::vec3{0.0f}
        : *event_position;
    if (engine.world_.registry().all_of<Transform>(*entity) &&
        event_position == nullptr) {
        position = engine.world_.registry().get<Transform>(*entity).position;
    }
    glm::vec3 direction{0.0f};
    if (instigator != 0u && engine.world_.registry().all_of<Transform>(*entity)) {
        const std::optional<entt::entity> instigator_entity =
            engine.world_.find_entity(instigator);
        if (instigator_entity.has_value() &&
            engine.world_.registry().all_of<Transform>(*instigator_entity)) {
            const glm::vec3 offset =
                engine.world_.registry().get<Transform>(*entity).position -
                engine.world_.registry().get<Transform>(*instigator_entity).position;
            if (glm::length(offset) > 0.0001f) {
                direction = glm::normalize(offset);
            }
        }
    }
    PeerId owner_peer = 0u;
    if (engine.world_.registry().all_of<NetworkIdentity>(*entity)) {
        owner_peer =
            engine.world_.registry().get<NetworkIdentity>(*entity).owner_peer;
    }
    if (engine.world_.registry().all_of<EntityKind>(*entity)) {
        const EntityKind& kind = engine.world_.registry().get<EntityKind>(*entity);
        entity_type = static_cast<std::uint16_t>(kind.type);
        actor_type = static_cast<std::uint16_t>(kind.actor_type);
    }
    std::vector<ActionGraphQueuedTrigger> queued_triggers;
    if (engine.world_.registry().all_of<
            OnDestroyEntityTriggerTag,
            ActionGraphDestroyEntityBinding>(*entity)) {
        const ActionGraphDestroyEntityBinding& binding =
            engine.world_.registry().get<ActionGraphDestroyEntityBinding>(*entity);
        queued_triggers.push_back(ActionGraphQueuedTrigger{
            binding.binding,
            net_id,
            TriggerEvent{
                TriggerEventType::kDestroyEntity,
                net_id,
                instigator,
                0u,
                position,
                direction,
                std::nullopt,
            },
            ActionExecutionProvenance{
                action_trigger_request_id(
                    engine.tick_loop_.current_tick(),
                    TriggerEventType::kDestroyEntity,
                    net_id,
                    instigator,
                    reason),
                0u,
                engine.tick_loop_.current_tick(),
                instigator,
                owner_peer,
                source_code,
                ActionAuthoritySource::kAuthoritativeSimulation,
            },
            reason,
        });
    }
    if (engine.physics_world_ != nullptr) {
        engine.physics_world_->remove_character(net_id);
    }
    if (!engine.world_.destroy(net_id)) {
        return false;
    }
    engine.pending_first_physics_actors_.erase(net_id);
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
    std::vector<ActionGraphCommandBatch> command_batches;
    if (dispatch_action_graph_triggers(
            &queued_triggers, &command_batches, nullptr)) {
        for (const ActionGraphCommandBatch& batch : command_batches) {
            (void)execute_action_graph_commands(
                engine,
                engine.world_,
                &engine.damage_pipeline_,
                engine.entity_templates_,
                batch,
                engine.current_server_time_us());
        }
    }
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
