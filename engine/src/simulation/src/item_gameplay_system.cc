#include "simulation/src/item_gameplay_system.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/kernel.h"
#include "physics/public/physics_world.h"
#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

namespace network_example {
namespace {

KernelGameplayRequestOutcome outcome_for(
    const KernelGameplayRequest& request) {
    KernelGameplayRequestOutcome outcome{};
    outcome.struct_size = sizeof(outcome);
    outcome.requester_peer = request.requester_peer;
    outcome.request_id = request.request_id;
    outcome.status = KernelGameplayRequestStatus_Rejected;
    outcome.graph_outcome = KernelGameplayGraphOutcome_NotSubmitted;
    outcome.rejection_reason = KernelGameplayRequestRejection_InvalidRequest;
    outcome.item_instance_id = request.selected_item_instance_id;
    return outcome;
}

std::uint8_t inventory_mapping(
    const KernelItemTemplateDefinition& definition,
    std::uint8_t button) {
    if (button == KernelSemanticInputButton_Use) {
        return definition.input_mapping.inventory_use;
    }
    if (button == KernelSemanticInputButton_Fire) {
        return definition.input_mapping.inventory_fire;
    }
    return KernelDomainAction_None;
}

std::uint8_t world_mapping(
    const KernelItemTemplateDefinition& definition,
    std::uint8_t button) {
    if (button == KernelSemanticInputButton_InteractTap) {
        return definition.input_mapping.world_interact_tap;
    }
    if (button == KernelSemanticInputButton_InteractHold) {
        return definition.input_mapping.world_interact_hold;
    }
    return KernelDomainAction_None;
}

std::uint8_t pure_prop_mapping(
    const KernelPropInteractionDefinition& definition,
    std::uint8_t button) {
    if (button == KernelSemanticInputButton_InteractTap) {
        return definition.world_interact_tap;
    }
    if (button == KernelSemanticInputButton_InteractHold) {
        return definition.world_interact_hold;
    }
    return KernelDomainAction_None;
}

std::uint8_t carrying_mapping(std::uint8_t button) {
    if (button == KernelSemanticInputButton_Fire) {
        return KernelDomainAction_Throw;
    }
    if (button == KernelSemanticInputButton_InteractTap) {
        return KernelDomainAction_Place;
    }
    return KernelDomainAction_None;
}

const KernelEntityTemplateDefinition* find_entity_template(
    const KernelEngine& engine,
    std::uint32_t id) {
    const auto found = std::find_if(
        engine.authored_entity_templates().begin(),
        engine.authored_entity_templates().end(),
        [id](const KernelEntityTemplateDefinition& candidate) {
            return candidate.entity_template_id == id;
        });
    return found == engine.authored_entity_templates().end() ? nullptr : &*found;
}

bool actor_owns_item(
    const KernelEngine& engine,
    const ItemInstanceRecord& item,
    std::uint32_t actor) {
    if (item.residency.kind != KernelItemResidency_Inventory) {
        return false;
    }
    const InventoryContainerRecord* container =
        engine.item_store().find_container(item.residency.container_id);
    return container != nullptr && container->owner_entity_id == actor;
}

bool within_range(
    const KernelEngine& engine,
    std::uint32_t actor_id,
    std::uint32_t target_id,
    float range) {
    const std::optional<entt::entity> actor =
        engine.simulation_world().find_entity(actor_id);
    const std::optional<entt::entity> target =
        engine.simulation_world().find_entity(target_id);
    if (!actor.has_value() || !target.has_value() || range <= 0.0f ||
        !engine.simulation_world().registry().all_of<Transform>(*actor) ||
        !engine.simulation_world().registry().all_of<Transform>(*target)) {
        return false;
    }
    const glm::vec3 delta =
        engine.simulation_world().registry().get<Transform>(*target).position -
        engine.simulation_world().registry().get<Transform>(*actor).position;
    return glm::dot(delta, delta) <= range * range;
}

bool placement_within_range(
    const KernelEngine& engine,
    std::uint32_t actor_id,
    const KernelVec3& position,
    float range) {
    const std::optional<entt::entity> actor =
        engine.simulation_world().find_entity(actor_id);
    if (!actor.has_value() ||
        !engine.simulation_world().registry().all_of<Transform>(*actor) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return false;
    }
    const glm::vec3 requested{position.x, position.y, position.z};
    const glm::vec3 delta = requested -
        engine.simulation_world().registry().get<Transform>(*actor).position;
    return glm::dot(delta, delta) <= range * range;
}

bool placement_is_clear(
    const KernelEngine& engine,
    const KernelVec3& placement,
    NetId ignored_entity = 0u) {
    const glm::vec3 point{placement.x, placement.y, placement.z};
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z)) {
        return false;
    }
    constexpr glm::vec3 kPlacementClearance{0.25f};
    for (const ColliderInstance& collider :
         engine.simulation_world().collider_registry().instances()) {
        if (!collider.enabled || collider.entity_net_id == ignored_entity) {
            continue;
        }
        const glm::vec3 minimum =
            collider.world_bounds.center - collider.world_bounds.half_extents -
            kPlacementClearance;
        const glm::vec3 maximum =
            collider.world_bounds.center + collider.world_bounds.half_extents +
            kPlacementClearance;
        if (point.x >= minimum.x && point.x <= maximum.x &&
            point.y >= minimum.y && point.y <= maximum.y &&
            point.z >= minimum.z && point.z <= maximum.z) {
            return false;
        }
    }
    if (engine.has_static_collision_scene() && engine.physics_world() != nullptr) {
        physics::RayCastRequest ground{};
        ground.origin = point + glm::vec3{0.0f, 0.5f, 0.0f};
        ground.direction = glm::vec3{0.0f, -1.0f, 0.0f};
        ground.max_distance = 1.0f;
        ground.filter.collision_mask =
            physics::collision_layer_bit(physics::CollisionLayer::kTerrain) |
            physics::collision_layer_bit(
                physics::CollisionLayer::kStaticObstacle);
        ground.filter.object_kind_mask =
            (1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kTerrain)) |
            (1u << static_cast<std::uint32_t>(
                physics::CollisionObjectKind::kStaticObstacle));
        physics::CollisionHit hit{};
        if (!engine.physics_world()->ray_cast_closest(ground, &hit)) {
            return false;
        }
    }
    return true;
}

void set_prop_collision_enabled(
    KernelEngine& engine,
    NetId prop_entity_id,
    bool enabled) {
    for (ColliderInstance& collider :
         engine.simulation_world().collider_registry().mutable_instances()) {
        if (collider.entity_net_id != prop_entity_id) {
            continue;
        }
        collider.enabled = enabled;
        if (engine.mutable_physics_world() != nullptr) {
            engine.mutable_physics_world()->set_object_enabled(
                collider.collider_id, enabled);
        }
    }
}

bool has_line_of_sight(
    const KernelEngine& engine,
    std::uint32_t actor_id,
    std::uint32_t target_id,
    std::uint32_t blocking_mask) {
    const physics::PhysicsWorld* physics_world = engine.physics_world();
    if (physics_world == nullptr) return true;
    const std::optional<entt::entity> actor =
        engine.simulation_world().find_entity(actor_id);
    const std::optional<entt::entity> target =
        engine.simulation_world().find_entity(target_id);
    if (!actor.has_value() || !target.has_value()) return false;
    const glm::vec3 origin =
        engine.simulation_world().registry().get<Transform>(*actor).position;
    const glm::vec3 destination =
        engine.simulation_world().registry().get<Transform>(*target).position;
    const glm::vec3 displacement = destination - origin;
    const float distance = glm::length(displacement);
    if (distance == 0.0f) return true;
    physics::RayCastRequest ray;
    ray.origin = origin;
    ray.direction = displacement / distance;
    ray.max_distance = distance;
    ray.filter.collision_mask = blocking_mask;
    ray.filter.ignored_entity_net_id = actor_id;
    const std::vector<physics::CollisionHit> hits =
        physics_world->ray_cast_all(ray);
    return hits.empty() || hits.front().identity.entity_net_id == target_id;
}

bool decorate_item_prop(
    KernelEngine& engine,
    std::uint32_t prop_id,
    const ItemInstanceRecord& item) {
    const std::optional<entt::entity> entity =
        engine.simulation_world().find_entity(prop_id);
    if (!entity.has_value()) return false;
    entt::registry& registry = engine.simulation_world().registry();
    registry.emplace_or_replace<ItemTemplateRef>(
        *entity,
        ItemTemplateRef{item.item_template_id});
    registry.emplace_or_replace<ItemInstanceRef>(
        *entity,
        ItemInstanceRef{item.item_instance_id});
    registry.emplace_or_replace<PropWorldMode>(
        *entity,
        PropWorldMode{static_cast<PropMode>(item.residency.world_mode)});
    return true;
}

std::optional<std::uint32_t> spawn_prop(
    KernelEngine& engine,
    const KernelItemTemplateDefinition& item_template,
    const KernelVec3& position) {
    if (item_template.entity_template_id == 0) return std::nullopt;
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_template_id = item_template.entity_template_id;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t prop_id = 0;
    if (!EntityLifecycleSystem{}.create_entity(
            engine, create, &prop_id, false)) {
        return std::nullopt;
    }
    return prop_id;
}

void reject(
    KernelGameplayRequestOutcome* outcome,
    KernelGameplayRequestRejectionReason reason) {
    outcome->status = KernelGameplayRequestStatus_Rejected;
    outcome->rejection_reason = static_cast<std::uint8_t>(reason);
}

std::optional<ActionGraphCommandBatch> prepare_item_graph_batch(
    KernelEngine& engine,
    const KernelGameplayRequest& request,
    const ItemInstanceRecord& item,
    const KernelItemTemplateDefinition& item_template,
    std::uint8_t domain_action,
    std::uint32_t quantity) {
    if (item_template.item_used_trigger.struct_size <
        sizeof(KernelActionTriggerDefinition)) {
        return std::nullopt;
    }
    const auto binding = compile_action_trigger_definition(
        TriggerEventType::kItemUsed,
        item_template.item_used_trigger);
    if (!binding.has_value()) return std::nullopt;
    const std::optional<entt::entity> actor =
        engine.simulation_world().find_entity(request.instigator_net_id);
    if (!actor.has_value()) return std::nullopt;
    const glm::vec3 position =
        engine.simulation_world().registry().get<Transform>(*actor).position;
    const TriggerEvent event{
        TriggerEventType::kItemUsed,
        0u,
        request.instigator_net_id,
        request.target_net_id,
        position,
        glm::vec3{
            request.throw_direction.x,
            request.throw_direction.y,
            request.throw_direction.z},
        std::nullopt,
        ItemUsedPayload{
            item.item_instance_id,
            item.item_template_id,
            domain_action,
            quantity,
        },
    };
    const ActionExecutionProvenance provenance{
        request.request_id,
        static_cast<std::uint32_t>(request.request_id),
        engine.current_tick(),
        request.instigator_net_id,
        request.requester_peer,
        0u,
        ActionAuthoritySource::kAuthoritativeSimulation,
        request.requester_peer,
    };
    std::vector<ActionGraphCommand> commands;
    if (!evaluate_action_graph(
            *binding,
            0u,
            event,
            provenance,
            &commands,
            nullptr)) {
        return std::nullopt;
    }
    for (const ActionGraphCommand& command : commands) {
        if (const auto* damage =
                std::get_if<ActionApplyDamageCommand>(&command)) {
            const std::optional<entt::entity> target =
                engine.simulation_world().find_entity(damage->target);
            if (!target.has_value() ||
                !engine.simulation_world().registry().all_of<Health>(*target)) {
                return std::nullopt;
            }
        }
    }
    return ActionGraphCommandBatch{
        event,
        provenance,
        0u,
        std::move(commands),
    };
}

}  // namespace

bool ItemGameplaySystem::submit_request(
    KernelEngine& engine,
    const KernelGameplayRequest& request) const {
    if (request.struct_size < sizeof(KernelGameplayRequest) ||
        request.request_id == 0 || request.instigator_net_id == 0 ||
        request.semantic_button > KernelSemanticInputButton_InteractHold) {
        return false;
    }
    const auto duplicate = std::find_if(
        engine.processed_gameplay_requests_.begin(),
        engine.processed_gameplay_requests_.end(),
        [&](const KernelGameplayRequestOutcome& candidate) {
            return candidate.requester_peer == request.requester_peer &&
                candidate.request_id == request.request_id;
        });
    if (duplicate != engine.processed_gameplay_requests_.end()) {
        engine.pending_gameplay_request_outcomes_.push_back(*duplicate);
        return true;
    }

    KernelGameplayRequestOutcome outcome = outcome_for(request);
    const std::optional<entt::entity> instigator =
        engine.world_.find_entity(request.instigator_net_id);
    if (!instigator.has_value()) {
        reject(&outcome, KernelGameplayRequestRejection_UnknownInstigator);
    } else if (!engine.world_.registry().all_of<NetworkIdentity>(*instigator) ||
               engine.world_.registry().get<NetworkIdentity>(*instigator)
                       .owner_peer != request.requester_peer) {
        reject(&outcome, KernelGameplayRequestRejection_NotAuthorized);
    } else {
        ItemInstanceRecord* item = request.selected_item_instance_id == 0
            ? nullptr
            : engine.item_store_.find_item(request.selected_item_instance_id);
        std::optional<entt::entity> target =
            request.target_net_id == 0
            ? std::nullopt
            : engine.world_.find_entity(request.target_net_id);
        if (item == nullptr && target.has_value() &&
            engine.world_.registry().all_of<ItemInstanceRef>(*target)) {
            const ItemInstanceRef& ref =
                engine.world_.registry().get<ItemInstanceRef>(*target);
            item = engine.item_store_.find_item(ref.item_instance_id);
            outcome.item_instance_id = ref.item_instance_id;
        }

        const KernelItemTemplateDefinition* item_template = item == nullptr
            ? nullptr
            : engine.item_store_.find_template(item->item_template_id);
        std::uint8_t action = KernelDomainAction_None;
        float interaction_range = 0.0f;
        std::uint32_t capabilities = 0;
        bool line_of_sight_required = false;
        std::uint32_t line_of_sight_blocking_mask = 0;
        if (item != nullptr && item_template != nullptr) {
            capabilities = item_template->capability_flags;
            interaction_range = item_template->interaction_range;
            line_of_sight_required = item_template->line_of_sight_required != 0;
            line_of_sight_blocking_mask =
                item_template->line_of_sight_blocking_mask;
            if (item->residency.kind == KernelItemResidency_Inventory) {
                if (!actor_owns_item(engine, *item, request.instigator_net_id)) {
                    reject(&outcome, KernelGameplayRequestRejection_NotAuthorized);
                    action = KernelDomainAction_None;
                    goto record_outcome;
                }
                action = inventory_mapping(*item_template, request.semantic_button);
            } else if (item->residency.kind == KernelItemResidency_World &&
                       item->residency.world_mode ==
                           KernelWorldItemMode_Carrying) {
                if (item->residency.carrier_entity_id !=
                    request.instigator_net_id) {
                    reject(&outcome, KernelGameplayRequestRejection_Claimed);
                    goto record_outcome;
                }
                action = carrying_mapping(request.semantic_button);
            } else if (item->residency.kind == KernelItemResidency_World &&
                       item->residency.world_mode == KernelWorldItemMode_Placed) {
                action = world_mapping(*item_template, request.semantic_button);
            } else {
                reject(&outcome, KernelGameplayRequestRejection_InvalidContext);
                goto record_outcome;
            }
        } else if (target.has_value() &&
                   engine.world_.registry().all_of<EntityTemplateRef>(*target)) {
            const std::uint32_t entity_template_id =
                engine.world_.registry().get<EntityTemplateRef>(*target)
                    .entity_template_id;
            const KernelEntityTemplateDefinition* entity_template =
                find_entity_template(engine, entity_template_id);
            if (entity_template != nullptr &&
                entity_template->entity_type == KernelEntityType_Prop) {
                capabilities = entity_template->prop.interaction.capability_flags;
                interaction_range =
                    entity_template->prop.interaction.interaction_range;
                line_of_sight_required =
                    entity_template->prop.interaction.line_of_sight_required != 0;
                line_of_sight_blocking_mask =
                    entity_template->prop.interaction
                        .line_of_sight_blocking_mask;
                const PropWorldMode* mode =
                    engine.world_.registry().try_get<PropWorldMode>(*target);
                if (mode != nullptr &&
                    mode->mode == PropMode::kCarrying) {
                    const CarriedBy* carried =
                        engine.world_.registry().try_get<CarriedBy>(*target);
                    if (carried == nullptr ||
                        carried->carrier_entity_id != request.instigator_net_id) {
                        reject(&outcome, KernelGameplayRequestRejection_Claimed);
                        goto record_outcome;
                    }
                    action = carrying_mapping(request.semantic_button);
                } else {
                    action = pure_prop_mapping(
                        entity_template->prop.interaction,
                        request.semantic_button);
                }
            }
        }

        outcome.domain_action = action;
        if (action == KernelDomainAction_None) {
            outcome.status = KernelGameplayRequestStatus_NoAction;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }
        if (line_of_sight_required && target.has_value() &&
            !has_line_of_sight(
                engine,
                request.instigator_net_id,
                request.target_net_id,
                line_of_sight_blocking_mask)) {
            reject(&outcome, KernelGameplayRequestRejection_LineOfSight);
            goto record_outcome;
        }
        if (action != KernelDomainAction_Consume &&
            action != KernelDomainAction_Place &&
            action != KernelDomainAction_Throw &&
            (!target.has_value() ||
             !within_range(
                 engine,
                 request.instigator_net_id,
                 request.target_net_id,
                 interaction_range))) {
            reject(&outcome, KernelGameplayRequestRejection_OutOfRange);
            goto record_outcome;
        }

        if (action == KernelDomainAction_Activate) {
            if ((capabilities & KernelItemCapability_Interactable) == 0 ||
                !target.has_value()) {
                reject(&outcome, KernelGameplayRequestRejection_MissingCapability);
                goto record_outcome;
            }
            KernelServerEntityActivateInfo activate{};
            activate.struct_size = sizeof(activate);
            activate.subject_net_id = request.target_net_id;
            activate.instigator_net_id = request.instigator_net_id;
            activate.target_net_id = request.target_net_id;
            activate.action_instance_id =
                static_cast<std::uint32_t>(request.request_id);
            activate.request_id = request.request_id;
            if (!ActivationSystem{}.activate_entity(engine, activate)) {
                reject(&outcome, KernelGameplayRequestRejection_GraphRejected);
                goto record_outcome;
            }
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.graph_outcome = KernelGameplayGraphOutcome_Succeeded;
            outcome.prop_entity_id = request.target_net_id;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }

        if (item == nullptr && target.has_value() &&
            action == KernelDomainAction_Carry) {
            if ((capabilities & KernelItemCapability_Carryable) == 0) {
                reject(&outcome, KernelGameplayRequestRejection_MissingCapability);
                goto record_outcome;
            }
            engine.world_.registry().emplace_or_replace<CarriedBy>(
                *target,
                CarriedBy{request.instigator_net_id});
            engine.world_.registry().emplace_or_replace<PropWorldMode>(
                *target,
                PropWorldMode{PropMode::kCarrying});
            engine.world_.registry().get_or_emplace<Velocity>(*target).linear =
                glm::vec3{0.0f};
            set_prop_collision_enabled(engine, request.target_net_id, false);
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.prop_entity_id = request.target_net_id;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }
        if (item == nullptr && target.has_value() &&
            (action == KernelDomainAction_Place ||
             action == KernelDomainAction_Throw)) {
            if (!placement_within_range(
                    engine,
                    request.instigator_net_id,
                    request.placement_position,
                    4.0f) && action == KernelDomainAction_Place) {
                reject(&outcome, KernelGameplayRequestRejection_InvalidPlacement);
                goto record_outcome;
            }
            if (action == KernelDomainAction_Place) {
                if (!placement_is_clear(
                        engine, request.placement_position, request.target_net_id)) {
                    reject(
                        &outcome,
                        KernelGameplayRequestRejection_InvalidPlacement);
                    goto record_outcome;
                }
                engine.world_.registry().get<Transform>(*target).position = glm::vec3{
                    request.placement_position.x,
                    request.placement_position.y,
                    request.placement_position.z};
                engine.world_.registry().emplace_or_replace<PropWorldMode>(
                    *target,
                    PropWorldMode{PropMode::kPlaced});
                set_prop_collision_enabled(engine, request.target_net_id, true);
            } else {
                glm::vec3 direction{
                    request.throw_direction.x,
                    request.throw_direction.y,
                    request.throw_direction.z};
                if ((capabilities & KernelItemCapability_Throwable) == 0 ||
                    glm::dot(direction, direction) == 0.0f) {
                    reject(&outcome, KernelGameplayRequestRejection_MissingCapability);
                    goto record_outcome;
                }
                direction = glm::normalize(direction);
                engine.world_.registry().emplace_or_replace<PropWorldMode>(
                    *target,
                    PropWorldMode{PropMode::kInFlight});
                engine.world_.registry().emplace_or_replace<Velocity>(
                    *target,
                    Velocity{direction * 10.0f});
                set_prop_collision_enabled(engine, request.target_net_id, true);
            }
            engine.world_.registry().remove<CarriedBy>(*target);
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.prop_entity_id = request.target_net_id;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }

        if (item == nullptr || item_template == nullptr) {
            reject(&outcome, KernelGameplayRequestRejection_UnknownItem);
            goto record_outcome;
        }

        if (action == KernelDomainAction_Consume) {
            std::optional<ActionGraphCommandBatch> item_graph_batch;
            if (item_template->item_used_trigger.struct_size >=
                sizeof(KernelActionTriggerDefinition)) {
                const auto binding = compile_action_trigger_definition(
                    TriggerEventType::kItemUsed,
                    item_template->item_used_trigger);
                if (!binding.has_value()) {
                    reject(&outcome, KernelGameplayRequestRejection_GraphRejected);
                    goto record_outcome;
                }
                const glm::vec3 position =
                    engine.world_.registry().get<Transform>(*instigator).position;
                const TriggerEvent event{
                    TriggerEventType::kItemUsed,
                    0u,
                    request.instigator_net_id,
                    request.target_net_id,
                    position,
                    glm::vec3{
                        request.throw_direction.x,
                        request.throw_direction.y,
                        request.throw_direction.z},
                    std::nullopt,
                    ItemUsedPayload{
                        item->item_instance_id,
                        item->item_template_id,
                        KernelDomainAction_Consume,
                        item_template->use_policy.quantity_cost,
                    },
                };
                const ActionExecutionProvenance provenance{
                    request.request_id,
                    static_cast<std::uint32_t>(request.request_id),
                    engine.tick_loop_.current_tick(),
                    request.instigator_net_id,
                    request.requester_peer,
                    0u,
                    ActionAuthoritySource::kAuthoritativeSimulation,
                    request.requester_peer,
                };
                std::vector<ActionGraphCommand> commands;
                if (!evaluate_action_graph(
                        *binding,
                        0u,
                        event,
                        provenance,
                        &commands,
                        nullptr)) {
                    reject(&outcome, KernelGameplayRequestRejection_GraphRejected);
                    goto record_outcome;
                }
                for (const ActionGraphCommand& command : commands) {
                    if (const auto* damage =
                            std::get_if<ActionApplyDamageCommand>(&command)) {
                        const std::optional<entt::entity> damage_target =
                            engine.world_.find_entity(damage->target);
                        if (!damage_target.has_value() ||
                            !engine.world_.registry().all_of<Health>(
                                *damage_target)) {
                            reject(
                                &outcome,
                                KernelGameplayRequestRejection_GraphRejected);
                            goto record_outcome;
                        }
                    }
                }
                item_graph_batch = ActionGraphCommandBatch{
                    event,
                    provenance,
                    0u,
                    std::move(commands),
                };
            }
            const auto consumed = engine.item_store_.consume(
                item->item_instance_id,
                engine.tick_loop_.current_tick());
            if (!consumed.has_value()) {
                reject(&outcome, KernelGameplayRequestRejection_InvalidQuantity);
                goto record_outcome;
            }
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.committed_quantity = consumed->committed_quantity;
            if (item_graph_batch.has_value()) {
                outcome.graph_outcome = execute_action_graph_command_batch(
                    engine,
                    *item_graph_batch,
                    engine.current_server_time_us())
                    ? KernelGameplayGraphOutcome_Succeeded
                    : KernelGameplayGraphOutcome_FailedAfterCommit;
            } else {
                outcome.graph_outcome =
                    KernelGameplayGraphOutcome_NotSubmitted;
            }
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }

        if (action == KernelDomainAction_Pickup) {
            const InventoryContainerRecord* container =
                engine.item_store_.find_container_for_owner(
                    request.instigator_net_id);
            if (container == nullptr) {
                reject(&outcome, KernelGameplayRequestRejection_InventoryFull);
                goto record_outcome;
            }
            const KernelItemInstanceId source_item_id = item->item_instance_id;
            const std::uint32_t source_quantity = item->quantity;
            const std::vector<KernelPortableStateFieldDefinition>
                portable_state_before = item->portable_state;
            if (target.has_value() &&
                engine.world_.registry().all_of<Health>(*target)) {
                const std::uint16_t hp =
                    engine.world_.registry().get<Health>(*target).hp;
                for (KernelPortableStateFieldDefinition& field :
                     item->portable_state) {
                    if (field.world_projection ==
                        KernelPortableStateProjection_HealthCurrent) {
                        field.uint32_default = hp;
                    }
                }
            }
            const auto inventory_item =
                engine.item_store_.transfer_world_to_inventory(
                    source_item_id,
                    container->inventory_container_id);
            if (!inventory_item.has_value()) {
                ItemInstanceRecord* source =
                    engine.item_store_.find_item(source_item_id);
                if (source != nullptr && !source->terminal) {
                    source->portable_state = portable_state_before;
                }
                reject(&outcome, KernelGameplayRequestRejection_InventoryFull);
                goto record_outcome;
            }
            EntityLifecycleSystem{}.destroy_entity(
                engine,
                request.target_net_id,
                KernelDespawnReason_Destroyed);
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.item_instance_id = *inventory_item;
            outcome.committed_quantity = source_quantity;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }

        if (action == KernelDomainAction_Carry) {
            if ((capabilities & KernelItemCapability_Carryable) == 0 ||
                !engine.item_store_.set_world_mode(
                    item->item_instance_id,
                    KernelWorldItemMode_Carrying,
                    request.instigator_net_id)) {
                reject(&outcome, KernelGameplayRequestRejection_Claimed);
                goto record_outcome;
            }
            engine.world_.registry().emplace_or_replace<CarriedBy>(
                *target,
                CarriedBy{request.instigator_net_id});
            engine.world_.registry().emplace_or_replace<PropWorldMode>(
                *target,
                PropWorldMode{PropMode::kCarrying});
            engine.world_.registry().get_or_emplace<Velocity>(*target).linear =
                glm::vec3{0.0f};
            set_prop_collision_enabled(engine, request.target_net_id, false);
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.prop_entity_id = request.target_net_id;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }

        if (action == KernelDomainAction_Place) {
            if (!placement_within_range(
                    engine,
                    request.instigator_net_id,
                    request.placement_position,
                    4.0f) ||
                !placement_is_clear(
                    engine,
                    request.placement_position,
                    item->residency.kind == KernelItemResidency_World
                        ? item->residency.prop_entity_id
                        : 0u)) {
                reject(&outcome, KernelGameplayRequestRejection_InvalidPlacement);
                goto record_outcome;
            }
            if (item->residency.kind == KernelItemResidency_World) {
                const std::optional<entt::entity> prop =
                    engine.world_.find_entity(item->residency.prop_entity_id);
                if (!prop.has_value()) {
                    reject(&outcome, KernelGameplayRequestRejection_StaleReference);
                    goto record_outcome;
                }
                Transform& transform =
                    engine.world_.registry().get<Transform>(*prop);
                transform.position = glm::vec3{
                    request.placement_position.x,
                    request.placement_position.y,
                    request.placement_position.z};
                engine.item_store_.set_world_mode(
                    item->item_instance_id,
                    KernelWorldItemMode_Placed);
                engine.world_.registry().emplace_or_replace<PropWorldMode>(
                    *prop,
                    PropWorldMode{PropMode::kPlaced});
                engine.world_.registry().remove<CarriedBy>(*prop);
                set_prop_collision_enabled(
                    engine, item->residency.prop_entity_id, true);
                outcome.prop_entity_id = item->residency.prop_entity_id;
            } else {
                const std::uint32_t quantity = request.requested_quantity == 0u
                    ? item->quantity
                    : request.requested_quantity;
                if (quantity == 0u || quantity > item->quantity ||
                    (item_template->item_mode == KernelItemMode_Stateful &&
                     quantity != 1u)) {
                    reject(
                        &outcome,
                        KernelGameplayRequestRejection_InvalidQuantity);
                    goto record_outcome;
                }
                const auto prop_id = spawn_prop(
                    engine,
                    *item_template,
                    request.placement_position);
                if (!prop_id.has_value()) {
                    reject(&outcome, KernelGameplayRequestRejection_InvalidPlacement);
                    goto record_outcome;
                }
                KernelItemInstanceId placed_id = item->item_instance_id;
                const bool whole_stack = quantity == item->quantity;
                if (whole_stack) {
                    if (!engine.item_store_.move_to_world(
                            placed_id,
                            *prop_id,
                            KernelWorldItemMode_Placed)) {
                        EntityLifecycleSystem{}.destroy_entity(
                            engine, *prop_id, KernelDespawnReason_Destroyed);
                        reject(
                            &outcome,
                            KernelGameplayRequestRejection_InvalidPlacement);
                        goto record_outcome;
                    }
                } else {
                    const auto split = engine.item_store_.split_to_world(
                        placed_id,
                        quantity,
                        *prop_id,
                        KernelWorldItemMode_Placed);
                    if (!split.has_value()) {
                        EntityLifecycleSystem{}.destroy_entity(
                            engine, *prop_id, KernelDespawnReason_Destroyed);
                        reject(
                            &outcome,
                            KernelGameplayRequestRejection_InvalidQuantity);
                        goto record_outcome;
                    }
                    placed_id = *split;
                }
                item = engine.item_store_.find_item(placed_id);
                decorate_item_prop(engine, *prop_id, *item);
                outcome.prop_entity_id = *prop_id;
                outcome.item_instance_id = placed_id;
                outcome.committed_quantity = quantity;
            }
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }

        if (action == KernelDomainAction_Throw) {
            if ((capabilities & KernelItemCapability_Throwable) == 0) {
                reject(&outcome, KernelGameplayRequestRejection_MissingCapability);
                goto record_outcome;
            }
            glm::vec3 direction{
                request.throw_direction.x,
                request.throw_direction.y,
                request.throw_direction.z};
            if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
                !std::isfinite(direction.z) || glm::dot(direction, direction) == 0.0f) {
                reject(&outcome, KernelGameplayRequestRejection_InvalidRequest);
                goto record_outcome;
            }
            direction = glm::normalize(direction);
            if (item_template->throw_policy.mode ==
                KernelItemThrowMode_ConsumeAndSpawn) {
                if (item->residency.kind != KernelItemResidency_Inventory) {
                    reject(&outcome, KernelGameplayRequestRejection_InvalidContext);
                    goto record_outcome;
                }
                const std::uint32_t quantity = request.requested_quantity == 0
                    ? 1u
                    : request.requested_quantity;
                const auto graph_batch = prepare_item_graph_batch(
                    engine,
                    request,
                    *item,
                    *item_template,
                    KernelDomainAction_Throw,
                    quantity);
                if (!graph_batch.has_value()) {
                    reject(&outcome, KernelGameplayRequestRejection_GraphRejected);
                    goto record_outcome;
                }
                const auto consumed = engine.item_store_.consume_quantity(
                    item->item_instance_id,
                    quantity,
                    engine.tick_loop_.current_tick());
                if (!consumed.has_value()) {
                    reject(&outcome, KernelGameplayRequestRejection_InvalidQuantity);
                    goto record_outcome;
                }
                outcome.status = KernelGameplayRequestStatus_Committed;
                outcome.committed_quantity = consumed->committed_quantity;
                outcome.graph_outcome = execute_action_graph_command_batch(
                    engine,
                    *graph_batch,
                    engine.current_server_time_us())
                    ? KernelGameplayGraphOutcome_Succeeded
                    : KernelGameplayGraphOutcome_FailedAfterCommit;
                outcome.rejection_reason = KernelGameplayRequestRejection_None;
                goto record_outcome;
            }
            if (item_template->throw_policy.mode !=
                KernelItemThrowMode_IdentityPreserving) {
                reject(&outcome, KernelGameplayRequestRejection_InvalidContext);
                goto record_outcome;
            }
            std::uint32_t prop_id = item->residency.prop_entity_id;
            if (item->residency.kind == KernelItemResidency_Inventory) {
                const std::uint32_t quantity = request.requested_quantity == 0
                    ? 1u
                    : request.requested_quantity;
                if (quantity == 0u || quantity > item->quantity ||
                    (item_template->item_mode == KernelItemMode_Stateful &&
                     quantity != 1u)) {
                    reject(&outcome, KernelGameplayRequestRejection_InvalidQuantity);
                    goto record_outcome;
                }
                const Transform& actor_transform =
                    engine.world_.registry().get<Transform>(*instigator);
                const KernelVec3 position{
                    actor_transform.position.x,
                    actor_transform.position.y,
                    actor_transform.position.z};
                const auto spawned = spawn_prop(engine, *item_template, position);
                if (!spawned.has_value()) {
                    reject(&outcome, KernelGameplayRequestRejection_InvalidPlacement);
                    goto record_outcome;
                }
                prop_id = *spawned;
                KernelItemInstanceId thrown_id = item->item_instance_id;
                if (quantity < item->quantity) {
                    const auto split = engine.item_store_.split_to_world(
                        item->item_instance_id,
                        quantity,
                        prop_id,
                        KernelWorldItemMode_InFlight);
                    if (!split.has_value()) {
                        EntityLifecycleSystem{}.destroy_entity(
                            engine, prop_id, KernelDespawnReason_Destroyed);
                        reject(&outcome, KernelGameplayRequestRejection_InvalidQuantity);
                        goto record_outcome;
                    }
                    thrown_id = *split;
                } else if (quantity == item->quantity) {
                    if (!engine.item_store_.move_to_world(
                            item->item_instance_id,
                            prop_id,
                            KernelWorldItemMode_InFlight)) {
                        EntityLifecycleSystem{}.destroy_entity(
                            engine, prop_id, KernelDespawnReason_Destroyed);
                        reject(&outcome, KernelGameplayRequestRejection_InvalidQuantity);
                        goto record_outcome;
                    }
                } else {
                    reject(&outcome, KernelGameplayRequestRejection_InvalidQuantity);
                    goto record_outcome;
                }
                item = engine.item_store_.find_item(thrown_id);
                decorate_item_prop(engine, prop_id, *item);
                outcome.item_instance_id = thrown_id;
                outcome.committed_quantity = quantity;
            } else {
                engine.item_store_.set_world_mode(
                    item->item_instance_id,
                    KernelWorldItemMode_InFlight);
                const std::optional<entt::entity> prop =
                    engine.world_.find_entity(prop_id);
                engine.world_.registry().remove<CarriedBy>(*prop);
                engine.world_.registry().emplace_or_replace<PropWorldMode>(
                    *prop,
                    PropWorldMode{PropMode::kInFlight});
            }
            const std::optional<entt::entity> prop =
                engine.world_.find_entity(prop_id);
            engine.world_.registry().emplace_or_replace<Velocity>(
                *prop,
                Velocity{direction * item_template->throw_policy.speed});
            set_prop_collision_enabled(engine, prop_id, true);
            outcome.status = KernelGameplayRequestStatus_Committed;
            outcome.prop_entity_id = prop_id;
            outcome.rejection_reason = KernelGameplayRequestRejection_None;
            goto record_outcome;
        }
    }

record_outcome:
    engine.processed_gameplay_requests_.push_back(outcome);
    engine.pending_gameplay_request_outcomes_.push_back(outcome);
    return true;
}

void ItemGameplaySystem::update_carried_props(KernelEngine& engine) const {
    auto view = engine.world_.registry().view<
        Transform,
        CarriedBy,
        EntityTemplateRef,
        PropWorldMode>();
    for (const entt::entity entity : view) {
        const PropWorldMode& mode = view.get<PropWorldMode>(entity);
        if (mode.mode != PropMode::kCarrying) continue;
        const CarriedBy& carried = view.get<CarriedBy>(entity);
        const std::optional<entt::entity> carrier =
            engine.world_.find_entity(carried.carrier_entity_id);
        if (!carrier.has_value() ||
            !engine.world_.registry().all_of<Transform>(*carrier)) {
            continue;
        }
        const EntityTemplateRef& template_ref =
            view.get<EntityTemplateRef>(entity);
        const KernelEntityTemplateDefinition* entity_template =
            find_entity_template(engine, template_ref.entity_template_id);
        const glm::vec3 offset = entity_template == nullptr
            ? glm::vec3{0.0f}
            : glm::vec3{
                  entity_template->prop.carry_offset_x,
                  entity_template->prop.carry_offset_y,
                  entity_template->prop.carry_offset_z};
        view.get<Transform>(entity).position =
            engine.world_.registry().get<Transform>(*carrier).position + offset;
    }
}

}  // namespace network_example
