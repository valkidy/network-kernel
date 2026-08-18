#include "simulation/src/systems.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>

#include "kernel/src/kernel.h"
#include "simulation/public/action_graph.h"
#include "simulation/public/collision_filter.h"
#include "simulation/src/item_gameplay_system.h"

namespace network_example {

bool execute_status_lifecycle_trigger(
    KernelEngine& engine,
    NetId target,
    NetId source,
    PeerId source_peer,
    std::uint32_t status_instance_id,
    std::uint16_t stack_count,
    TriggerEventType event_type,
    const std::optional<CompiledActionGraphBinding>& binding,
    std::uint64_t server_time_us);

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
    std::uint32_t sequence,
    std::uint32_t discriminator = 0u) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint32_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(server_tick);
    mix(subject);
    mix(related_entity);
    mix(sequence);
    mix(discriminator);
    mix(static_cast<std::uint32_t>(event_type));
    return hash;
}

World::ActionGraphDedupKey action_graph_dedup_key(
    const ActionGraphCommandBatch& batch) {
    return World::ActionGraphDedupKey{
        batch.provenance.requester_peer != 0u
            ? batch.provenance.requester_peer
            : batch.provenance.owner_peer,
        batch.provenance.request_id,
        batch.event.type,
        batch.sequence,
    };
}

class ActionGraphDedupTransaction {
public:
    explicit ActionGraphDedupTransaction(World& world) : world_(world) {}

    ~ActionGraphDedupTransaction() {
        if (!active_) {
            return;
        }
        for (const World::ActionGraphDedupKey& key : reservations_) {
            world_.cancel_action_graph_batch(key);
        }
        world_.release_action_graph_batch_capacity(capacity_remaining_);
    }

    bool is_reserved(const World::ActionGraphDedupKey& key) const {
        return std::find(reservations_.begin(), reservations_.end(), key) !=
            reservations_.end();
    }

    bool reserve_all(
        const std::vector<const ActionGraphCommandBatch*>& batches) {
        std::vector<World::ActionGraphDedupKey> new_keys;
        new_keys.reserve(batches.size());
        for (const ActionGraphCommandBatch* batch : batches) {
            if (batch == nullptr) {
                return false;
            }
            const World::ActionGraphDedupKey key =
                action_graph_dedup_key(*batch);
            if (world_.action_graph_batch_processed(
                    key.requester_peer,
                    key.request_id,
                    key.event_type,
                    key.sequence) ||
                is_reserved(key) ||
                std::find(new_keys.begin(), new_keys.end(), key) !=
                    new_keys.end()) {
                continue;
            }
            new_keys.push_back(key);
        }
        if (!world_.reserve_action_graph_batch_capacity(new_keys.size())) {
            return false;
        }
        capacity_remaining_ += new_keys.size();
        for (const ActionGraphCommandBatch* batch : batches) {
            if (!reserve(action_graph_dedup_key(*batch))) {
                return false;
            }
        }
        return true;
    }

    bool reserve(const World::ActionGraphDedupKey& key) {
        const World::ActionGraphDedupReservationResult result =
            world_.reserve_action_graph_batch(key);
        if (result == World::ActionGraphDedupReservationResult::kRejected) {
            return false;
        }
        if (result == World::ActionGraphDedupReservationResult::kReserved) {
            reservations_.push_back(key);
            if (capacity_remaining_ > 0u) {
                --capacity_remaining_;
            }
        }
        return true;
    }

    bool commit(std::uint32_t committed_tick) {
        for (const World::ActionGraphDedupKey& key : reservations_) {
            if (!world_.commit_action_graph_batch(key, committed_tick)) {
                return false;
            }
        }
        world_.release_action_graph_batch_capacity(capacity_remaining_);
        active_ = false;
        return true;
    }

private:
    World& world_;
    std::vector<World::ActionGraphDedupKey> reservations_;
    std::size_t capacity_remaining_ = 0u;
    bool active_ = true;
};

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

glm::vec3 normalized_direction_or_zero(const glm::vec3& direction) {
    const float length_squared = glm::dot(direction, direction);
    if (!std::isfinite(length_squared) || length_squared <= 0.00000001f) {
        return glm::vec3{0.0f};
    }
    return direction / std::sqrt(length_squared);
}

KernelQuat yaw_rotation_from_direction(const glm::vec3& direction) {
    const glm::vec3 horizontal =
        normalized_direction_or_zero(glm::vec3{direction.x, 0.0f, direction.z});
    if (horizontal == glm::vec3{0.0f}) {
        return KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float yaw = std::atan2(horizontal.x, horizontal.z);
    const glm::quat rotation =
        glm::angleAxis(yaw, glm::vec3{0.0f, 1.0f, 0.0f});
    return KernelQuat{
        rotation.x,
        rotation.y,
        rotation.z,
        rotation.w,
    };
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

void recompute_speed(World& world, entt::entity entity) {
    if (!world.registry().all_of<MovementState>(entity)) {
        return;
    }
    MovementState& movement = world.registry().get<MovementState>(entity);
    const StatusEffectState* status =
        world.registry().try_get<StatusEffectState>(entity);
    float multiplier = 1.0f;
    float additive = 0.0f;
    if (status != nullptr) {
        for (const SpeedModifier& modifier : status->speed_modifiers) {
            multiplier *= modifier.multiplier;
            additive += modifier.additive;
        }
    }
    movement.speed_meters_per_second =
        movement.base_speed_meters_per_second * multiplier + additive;
}

void advance_status_revision(StatusEffectState& state) {
    ++state.revision;
    if (state.revision == 0u) {
        ++state.revision;
    }
}

struct PreparedStatusLifecycle {
    std::optional<ActionGraphCommandBatch> batch;
};

bool prepare_status_lifecycle_trigger(
    KernelEngine& engine,
    NetId target,
    NetId source,
    PeerId source_peer,
    std::uint32_t status_instance_id,
    std::uint16_t stack_count,
    TriggerEventType event_type,
    const std::optional<CompiledActionGraphBinding>& binding,
    PreparedStatusLifecycle* out_prepared) {
    if (out_prepared == nullptr || target == 0u || source == 0u ||
        status_instance_id == 0u || stack_count == 0u || stack_count > 32u) {
        return false;
    }
    out_prepared->batch.reset();
    if (!binding.has_value()) {
        return true;
    }
    TriggerEvent event;
    event.type = event_type;
    event.subject = target;
    event.instigator = source;
    event.target = target;
    ActionExecutionProvenance provenance;
    provenance.request_id = action_trigger_request_id(
        engine.current_tick(),
        event_type,
        target,
        source,
        status_instance_id,
        stack_count);
    provenance.action_instance_id = status_instance_id;
    provenance.status_instance_id = status_instance_id;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.owner_peer = source_peer;
    provenance.authority_source =
        ActionAuthoritySource::kAuthoritativeSimulation;
    ActionGraphCommandBatch batch{
        event, provenance, status_instance_id, {}};
    std::string error;
    if (!evaluate_action_graph(
            *binding,
            target,
            event,
            provenance,
            &batch.commands,
            &error)) {
        return false;
    }
    World& world = engine.simulation_world();
    batch.commands.erase(
        std::remove_if(
            batch.commands.begin(),
            batch.commands.end(),
            [&](const ActionGraphCommand& command) {
                NetId side_effect_target = 0u;
                if (const auto* damage =
                        std::get_if<ActionApplyDamageCommand>(&command)) {
                    side_effect_target = damage->target;
                } else if (const auto* health =
                               std::get_if<ActionApplyHealthChangeCommand>(&command)) {
                    side_effect_target = health->target;
                } else if (const auto* modifier =
                               std::get_if<ActionApplySpeedModifierCommand>(&command)) {
                    if (event_type != TriggerEventType::kStatusApplied ||
                        modifier->target != target) {
                        side_effect_target = 0u;
                    } else {
                        side_effect_target = modifier->target;
                    }
                } else {
                    side_effect_target = 0u;
                }
                if (side_effect_target == 0u) {
                    return false;
                }
                const std::optional<entt::entity> entity =
                    world.find_entity(side_effect_target);
                return !entity.has_value() ||
                    !world.registry().all_of<NetworkIdentity>(*entity);
            }),
        batch.commands.end());
    const bool scale_amount =
        event_type == TriggerEventType::kStatusTick ||
        event_type == TriggerEventType::kStatusExpired;
    for (ActionGraphCommand& command : batch.commands) {
        if (auto* damage = std::get_if<ActionApplyDamageCommand>(&command);
            damage != nullptr && scale_amount) {
            const std::uint32_t scaled =
                static_cast<std::uint32_t>(damage->amount) * stack_count;
            if (scaled > UINT16_MAX) {
                return false;
            }
            damage->amount = static_cast<std::uint16_t>(scaled);
        } else if (auto* health =
                       std::get_if<ActionApplyHealthChangeCommand>(&command);
                   health != nullptr && scale_amount) {
            const std::int64_t scaled =
                static_cast<std::int64_t>(health->amount) * stack_count;
            if (scaled < INT32_MIN || scaled > INT32_MAX) {
                return false;
            }
            health->amount = static_cast<std::int32_t>(scaled);
        } else if (auto* modifier =
                       std::get_if<ActionApplySpeedModifierCommand>(&command)) {
            const double scaled =
                modifier->operation == KernelStatModifierOperation_Additive
                    ? static_cast<double>(modifier->value) * stack_count
                    : std::pow(
                          static_cast<double>(modifier->value), stack_count);
            if (!std::isfinite(scaled) ||
                std::abs(scaled) > std::numeric_limits<float>::max()) {
                return false;
            }
            modifier->value = static_cast<float>(scaled);
        }
    }
    for (const ActionGraphCommand& command : batch.commands) {
        if (!std::holds_alternative<ActionApplyDamageCommand>(command) &&
            !std::holds_alternative<ActionApplyHealthChangeCommand>(command) &&
            !std::holds_alternative<ActionApplySpeedModifierCommand>(command)) {
            return false;
        }
        if (const auto* modifier =
                std::get_if<ActionApplySpeedModifierCommand>(&command);
            modifier != nullptr &&
            (event_type != TriggerEventType::kStatusApplied ||
             modifier->target != target)) {
            return false;
        }
    }
    out_prepared->batch = std::move(batch);
    return true;
}

bool execute_action_graph_commands(
    KernelEngine& engine,
    World& world,
    DamagePipeline* damage_pipeline,
    const std::vector<KernelEntityTemplateDefinition>& entity_templates,
    const ActionGraphCommandBatch& batch,
    std::uint64_t server_time_us,
    ActionGraphDedupTransaction* transaction = nullptr) {
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
    std::vector<ActionGraphCommandBatch> lifecycle_batches;
    lifecycle_batches.reserve(commands.size());
    std::vector<std::uint32_t> planned_status_instance_ids(
        commands.size(), 0u);
    std::unordered_map<
        NetId,
        std::unordered_map<std::uint32_t, std::uint32_t>> projected_statuses;
    const auto projected_for = [&](NetId target)
        -> std::unordered_map<std::uint32_t, std::uint32_t>& {
        auto [found, inserted] = projected_statuses.try_emplace(target);
        if (inserted) {
            const std::optional<entt::entity> entity = world.find_entity(target);
            const StatusEffectState* state = entity.has_value()
                ? world.registry().try_get<StatusEffectState>(*entity)
                : nullptr;
            if (state != nullptr) {
                for (const ActiveStatusEffect& active : state->active) {
                    found->second[active.channel_id] = active.status_effect_id;
                }
            }
        }
        return found->second;
    };
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const ActionGraphCommand& command = commands[index];
        if (const auto* apply_status =
                std::get_if<ActionApplyStatusCommand>(&command)) {
            const RuntimeStatusEffectTemplate* status_template =
                world.find_status_effect_template(apply_status->status_effect_id);
            if (status_template == nullptr) {
                return false;
            }
            auto& projected = projected_for(apply_status->target);
            projected[status_template->channel_id] =
                status_template->status_effect_id;
            if (projected.size() > kMaxActiveStatusEffects) {
                return false;
            }
        } else if (const auto* remove_status =
                       std::get_if<ActionRemoveStatusCommand>(&command)) {
            auto& projected = projected_for(remove_status->target);
            std::erase_if(
                projected,
                [&](const auto& entry) {
                    return entry.second == remove_status->status_effect_id;
                });
        }
    }
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const ActionGraphCommand& command = commands[index];
        if (const auto* damage =
                std::get_if<ActionApplyDamageCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(damage->target);
            if (damage->source == 0u || !target.has_value() ||
                !world.registry().all_of<NetworkIdentity>(*target)) {
                return false;
            }
            continue;
        }
        if (const auto* health_change =
                std::get_if<ActionApplyHealthChangeCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(health_change->target);
            if (health_change->source == 0u || !target.has_value() ||
                !world.registry().all_of<NetworkIdentity>(*target)) {
                return false;
            }
            continue;
        }
        if (const auto* impulse =
                std::get_if<ActionApplyImpulseCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(impulse->target);
            if (impulse->source == 0u || !target.has_value() ||
                !std::isfinite(impulse->strength) || impulse->strength <= 0.0f ||
                !std::isfinite(impulse->direction.x) ||
                !std::isfinite(impulse->direction.y) ||
                !std::isfinite(impulse->direction.z) ||
                glm::dot(impulse->direction, impulse->direction) <= 0.0f) {
                return false;
            }
            const EntityKind& kind = world.registry().get<EntityKind>(*target);
            const bool actor_target = kind.type == EntityType::kActor &&
                (impulse->collision_mask & KERNEL_COLLISION_MASK_ACTOR) != 0u;
            const bool prop_target = kind.type == EntityType::kProp &&
                (impulse->collision_mask & KERNEL_COLLISION_MASK_PROP) != 0u &&
                world.registry().all_of<PropWorldMode>(*target) &&
                world.registry().get<PropWorldMode>(*target).mode !=
                    PropMode::kCarrying;
            if (!actor_target && !prop_target) {
                continue;
            }
            const ImpulseResistance* resistance =
                world.registry().try_get<ImpulseResistance>(*target);
            if (resistance != nullptr &&
                (!std::isfinite(resistance->value) ||
                 impulse->strength <= resistance->value)) {
                continue;
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
        if (const auto* apply_status =
                std::get_if<ActionApplyStatusCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(apply_status->target);
            const RuntimeStatusEffectTemplate* status_template =
                world.find_status_effect_template(apply_status->status_effect_id);
            if (apply_status->source == 0u || !target.has_value() ||
                status_template == nullptr) {
                return false;
            }
            const StatusEffectState* state =
                world.registry().try_get<StatusEffectState>(*target);
            const ActiveStatusEffect* same_status = nullptr;
            if (state != nullptr) {
                const auto found = std::find_if(
                    state->active.begin(),
                    state->active.end(),
                    [&](const ActiveStatusEffect& active) {
                        return active.channel_id == status_template->channel_id &&
                            active.status_effect_id ==
                                status_template->status_effect_id;
                    });
                same_status = found == state->active.end() ? nullptr : &*found;
            }
            PeerId source_peer = apply_status->provenance.owner_peer;
            const std::optional<entt::entity> source_entity =
                world.find_entity(apply_status->source);
            if (source_entity.has_value() &&
                world.registry().all_of<NetworkIdentity>(*source_entity)) {
                source_peer =
                    world.registry().get<NetworkIdentity>(*source_entity).owner_peer;
            }
            if (same_status != nullptr &&
                status_template->replacement_policy ==
                    KernelStatusEffectReplacementPolicy_Refresh) {
                continue;
            }
            if (same_status != nullptr &&
                status_template->replacement_policy ==
                    KernelStatusEffectReplacementPolicy_Stack) {
                if (same_status->stack_count >= status_template->max_stacks) {
                    continue;
                }
                PreparedStatusLifecycle prepared_stack;
                if (!prepare_status_lifecycle_trigger(
                        engine,
                        apply_status->target,
                        apply_status->source,
                        source_peer,
                        same_status->instance_id,
                        static_cast<std::uint16_t>(
                            same_status->stack_count + 1u),
                        TriggerEventType::kStatusApplied,
                        status_template->on_apply_binding,
                        &prepared_stack)) {
                    return false;
                }
                if (prepared_stack.batch.has_value()) {
                    lifecycle_batches.push_back(std::move(*prepared_stack.batch));
                }
                continue;
            }
            if (state != nullptr) {
                for (const ActiveStatusEffect& active : state->active) {
                    if (active.channel_id != status_template->channel_id) {
                        continue;
                    }
                    const RuntimeStatusEffectTemplate* old_template =
                        world.find_status_effect_template(active.status_effect_id);
                    PreparedStatusLifecycle prepared;
                    if (old_template != nullptr &&
                        !prepare_status_lifecycle_trigger(
                            engine,
                            apply_status->target,
                            active.source,
                            active.source_peer,
                            active.instance_id,
                            active.stack_count,
                            TriggerEventType::kStatusExpired,
                            old_template->on_expire_binding,
                            &prepared)) {
                        return false;
                    }
                    if (prepared.batch.has_value()) {
                        lifecycle_batches.push_back(std::move(*prepared.batch));
                    }
                }
            }
            const std::uint32_t planned_instance_id =
                world.allocate_status_instance_id();
            planned_status_instance_ids[index] = planned_instance_id;
            PreparedStatusLifecycle prepared_apply;
            if (!prepare_status_lifecycle_trigger(
                    engine,
                    apply_status->target,
                    apply_status->source,
                    source_peer,
                    planned_instance_id,
                    1u,
                    TriggerEventType::kStatusApplied,
                    status_template->on_apply_binding,
                    &prepared_apply)) {
                return false;
            }
            if (prepared_apply.batch.has_value()) {
                lifecycle_batches.push_back(std::move(*prepared_apply.batch));
            }
            continue;
        }
        if (const auto* remove_status =
                std::get_if<ActionRemoveStatusCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(remove_status->target);
            if (remove_status->source == 0u || !target.has_value() ||
                world.find_status_effect_template(remove_status->status_effect_id) == nullptr) {
                return false;
            }
            const StatusEffectState* state =
                world.registry().try_get<StatusEffectState>(*target);
            if (state != nullptr) {
                for (const ActiveStatusEffect& active : state->active) {
                    if (active.status_effect_id != remove_status->status_effect_id) {
                        continue;
                    }
                    const RuntimeStatusEffectTemplate* status_template =
                        world.find_status_effect_template(active.status_effect_id);
                    PreparedStatusLifecycle prepared;
                    if (status_template != nullptr &&
                        !prepare_status_lifecycle_trigger(
                            engine,
                            remove_status->target,
                            active.source,
                            active.source_peer,
                            active.instance_id,
                            active.stack_count,
                            TriggerEventType::kStatusExpired,
                            status_template->on_expire_binding,
                            &prepared)) {
                        return false;
                    }
                    if (prepared.batch.has_value()) {
                        lifecycle_batches.push_back(std::move(*prepared.batch));
                    }
                }
            }
            continue;
        }
        if (const auto* modifier =
                std::get_if<ActionApplySpeedModifierCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(modifier->target);
            if (modifier->source == 0u || !target.has_value() ||
                modifier->status_instance_id == 0u ||
                (modifier->operation != KernelStatModifierOperation_Additive &&
                 modifier->operation != KernelStatModifierOperation_Multiplier) ||
                !std::isfinite(modifier->value)) {
                return false;
            }
            continue;
        }
        const auto* spawn = std::get_if<ActionSpawnEntityCommand>(&command);
        const std::optional<entt::entity> owner = spawn == nullptr
            ? std::nullopt
            : world.find_entity(spawn->owner);
        const KernelEntityTemplateDefinition* entity_template = spawn == nullptr
            ? nullptr
            : find_entity_template(entity_templates, spawn->entity_template_id);
        if (spawn == nullptr ||
            entity_template == nullptr ||
            !owner.has_value() ||
            !world.registry().all_of<NetworkIdentity>(*owner) ||
            !std::isfinite(spawn->position.x) ||
            !std::isfinite(spawn->position.y) ||
            !std::isfinite(spawn->position.z)) {
            return false;
        }
        if (spawn->item_template_id != 0u) {
            const KernelItemTemplateDefinition* item_template =
                engine.item_store().find_template(spawn->item_template_id);
            if (item_template == nullptr || spawn->quantity == 0u ||
                item_template->entity_template_id != spawn->entity_template_id ||
                spawn->quantity > item_template->max_stack ||
                (item_template->item_mode == KernelItemMode_Stateful &&
                 spawn->quantity != 1u)) {
                return false;
            }
        } else if (spawn->quantity != 0u) {
            return false;
        }
    }
    std::optional<ActionGraphDedupTransaction> owned_transaction;
    ActionGraphDedupTransaction* active_transaction = transaction;
    if (active_transaction == nullptr) {
        owned_transaction.emplace(world);
        active_transaction = &*owned_transaction;
    }
    std::vector<const ActionGraphCommandBatch*> batches_to_reserve;
    batches_to_reserve.reserve(1u + lifecycle_batches.size());
    batches_to_reserve.push_back(&batch);
    for (const ActionGraphCommandBatch& lifecycle_batch : lifecycle_batches) {
        batches_to_reserve.push_back(&lifecycle_batch);
    }
    if (!active_transaction->reserve_all(batches_to_reserve)) {
        return false;
    }
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const ActionGraphCommand& command = commands[index];
        if (const auto* damage =
                std::get_if<ActionApplyDamageCommand>(&command)) {
            const std::optional<entt::entity> target =
                world.find_entity(damage->target);
            if (!target.has_value() ||
                !world.registry().all_of<Health>(*target)) {
                continue;
            }
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
        if (const auto* health_change =
                std::get_if<ActionApplyHealthChangeCommand>(&command)) {
            const std::optional<entt::entity> target_entity =
                world.find_entity(health_change->target);
            if (!target_entity.has_value() ||
                !world.registry().all_of<Health>(*target_entity)) {
                continue;
            }
            if (health_change->amount < 0) {
                const std::uint16_t damage_amount =
                    static_cast<std::uint16_t>(-health_change->amount);
                if (!damage_pipeline->submit_damage_request(DamageRequest{
                        health_change->provenance.server_tick,
                        static_cast<std::uint32_t>(index),
                        health_change->source,
                        health_change->target,
                        health_change->provenance.owner_peer,
                        0u,
                        damage_amount,
                        server_time_us,
                        batch.event.position,
                    })) {
                    return false;
                }
                continue;
            }
            const entt::entity target = *world.find_entity(health_change->target);
            Health& health = world.registry().get<Health>(target);
            if (health.hp == 0u || health.hp >= health.max_hp) {
                continue;
            }
            const std::uint32_t increased = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(health.max_hp - health.hp),
                static_cast<std::uint32_t>(health_change->amount));
            health.hp = static_cast<std::uint16_t>(health.hp + increased);
            engine.queue_health_changed_event(
                health_change->target,
                health_change->provenance.owner_peer,
                static_cast<std::int32_t>(increased),
                server_time_us);
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
        if (const auto* apply_status =
                std::get_if<ActionApplyStatusCommand>(&command)) {
            const entt::entity target = *world.find_entity(apply_status->target);
            StatusEffectState& status_state =
                world.registry().get_or_emplace<StatusEffectState>(target);
            const RuntimeStatusEffectTemplate* status_template =
                world.find_status_effect_template(apply_status->status_effect_id);
            if (status_template == nullptr) {
                return false;
            }
            PeerId source_peer = apply_status->provenance.owner_peer;
            const std::optional<entt::entity> source_entity =
                world.find_entity(apply_status->source);
            if (source_entity.has_value() &&
                world.registry().all_of<NetworkIdentity>(*source_entity)) {
                source_peer =
                    world.registry().get<NetworkIdentity>(*source_entity).owner_peer;
            }
            auto same_status = std::find_if(
                status_state.active.begin(),
                status_state.active.end(),
                [&](const ActiveStatusEffect& active) {
                    return active.channel_id == status_template->channel_id &&
                        active.status_effect_id ==
                            status_template->status_effect_id;
                });
            if (same_status != status_state.active.end() &&
                status_template->replacement_policy ==
                    KernelStatusEffectReplacementPolicy_Refresh) {
                same_status->source = apply_status->source;
                same_status->source_peer = source_peer;
                same_status->expire_tick =
                    engine.current_tick() + status_template->duration_ticks;
                advance_status_revision(status_state);
                engine.queue_status_effect_presentation(
                    apply_status->target,
                    same_status->status_effect_id,
                    same_status->instance_id,
                    KernelRemoteActionPresentationEventType_StatusUpdated,
                    same_status->stack_count);
                engine.publish_status_effect_state(apply_status->target);
                continue;
            }
            if (same_status != status_state.active.end() &&
                status_template->replacement_policy ==
                    KernelStatusEffectReplacementPolicy_Stack) {
                if (same_status->stack_count >= status_template->max_stacks) {
                    if (!status_template->refresh_on_stack) {
                        continue;
                    }
                    same_status->source = apply_status->source;
                    same_status->source_peer = source_peer;
                    same_status->expire_tick =
                        engine.current_tick() + status_template->duration_ticks;
                    advance_status_revision(status_state);
                    engine.queue_status_effect_presentation(
                        apply_status->target,
                        same_status->status_effect_id,
                        same_status->instance_id,
                        KernelRemoteActionPresentationEventType_StatusUpdated,
                        same_status->stack_count);
                    engine.publish_status_effect_state(apply_status->target);
                    continue;
                }
                const std::uint16_t next_stack_count =
                    static_cast<std::uint16_t>(same_status->stack_count + 1u);
                PreparedStatusLifecycle prepared_stack;
                if (!prepare_status_lifecycle_trigger(
                        engine,
                        apply_status->target,
                        apply_status->source,
                        source_peer,
                        same_status->instance_id,
                        next_stack_count,
                        TriggerEventType::kStatusApplied,
                        status_template->on_apply_binding,
                        &prepared_stack)) {
                    return false;
                }
                same_status->source = apply_status->source;
                same_status->source_peer = source_peer;
                same_status->stack_count = next_stack_count;
                if (status_template->refresh_on_stack) {
                    same_status->expire_tick =
                        engine.current_tick() + status_template->duration_ticks;
                }
                if (prepared_stack.batch.has_value() &&
                    !execute_action_graph_commands(
                        engine,
                        world,
                        damage_pipeline,
                        entity_templates,
                        *prepared_stack.batch,
                        server_time_us,
                        active_transaction)) {
                    return false;
                }
                advance_status_revision(status_state);
                engine.queue_status_effect_presentation(
                    apply_status->target,
                    same_status->status_effect_id,
                    same_status->instance_id,
                    KernelRemoteActionPresentationEventType_StatusUpdated,
                    same_status->stack_count);
                engine.publish_status_effect_state(apply_status->target);
                continue;
            }
            std::vector<ActiveStatusEffect> replaced;
            for (const ActiveStatusEffect& old_status : status_state.active) {
                if (old_status.channel_id == status_template->channel_id) {
                    replaced.push_back(old_status);
                }
            }
            if (status_state.active.size() - replaced.size() + 1u >
                kMaxActiveStatusEffects) {
                return false;
            }
            std::vector<PreparedStatusLifecycle> prepared_expire(replaced.size());
            for (std::size_t replaced_index = 0u;
                 replaced_index < replaced.size();
                 ++replaced_index) {
                const ActiveStatusEffect& old_status = replaced[replaced_index];
                const RuntimeStatusEffectTemplate* old_template =
                    world.find_status_effect_template(old_status.status_effect_id);
                if (old_template != nullptr &&
                    !prepare_status_lifecycle_trigger(
                        engine,
                        apply_status->target,
                        old_status.source,
                        old_status.source_peer,
                        old_status.instance_id,
                        old_status.stack_count,
                        TriggerEventType::kStatusExpired,
                        old_template->on_expire_binding,
                        &prepared_expire[replaced_index])) {
                    return false;
                }
            }
            const std::uint32_t applied_tick = engine.current_tick();
            const std::uint32_t instance_id = planned_status_instance_ids[index];
            const ActiveStatusEffect active{
                instance_id,
                status_template->status_effect_id,
                status_template->channel_id,
                apply_status->source,
                source_peer,
                applied_tick,
                applied_tick + status_template->duration_ticks,
                status_template->interval_ticks == 0u
                    ? 0u
                    : applied_tick + status_template->interval_ticks,
                1u,
            };
            PreparedStatusLifecycle prepared_apply;
            if (!prepare_status_lifecycle_trigger(
                    engine,
                    apply_status->target,
                    apply_status->source,
                    source_peer,
                    instance_id,
                    1u,
                    TriggerEventType::kStatusApplied,
                    status_template->on_apply_binding,
                    &prepared_apply)) {
                return false;
            }
            for (PreparedStatusLifecycle& prepared : prepared_expire) {
                if (prepared.batch.has_value() &&
                    !execute_action_graph_commands(
                        engine,
                        world,
                        damage_pipeline,
                        entity_templates,
                        *prepared.batch,
                        server_time_us,
                        active_transaction)) {
                    return false;
                }
            }
            for (const ActiveStatusEffect& old_status : replaced) {
                status_state.speed_modifiers.erase(
                    std::remove_if(
                        status_state.speed_modifiers.begin(),
                        status_state.speed_modifiers.end(),
                        [&](const SpeedModifier& modifier) {
                            return modifier.status_instance_id ==
                                old_status.instance_id;
                        }),
                    status_state.speed_modifiers.end());
            }
            status_state.active.erase(
                std::remove_if(
                    status_state.active.begin(),
                    status_state.active.end(),
                    [&](const ActiveStatusEffect& old_status) {
                        return old_status.channel_id == status_template->channel_id;
                    }),
                status_state.active.end());
            recompute_speed(world, target);
            status_state.active.push_back(active);
            std::sort(
                status_state.active.begin(), status_state.active.end(),
                [](const ActiveStatusEffect& lhs, const ActiveStatusEffect& rhs) {
                    return lhs.instance_id < rhs.instance_id;
                });
            if (prepared_apply.batch.has_value() &&
                !execute_action_graph_commands(
                    engine,
                    world,
                    damage_pipeline,
                    entity_templates,
                    *prepared_apply.batch,
                    server_time_us,
                    active_transaction)) {
                return false;
            }
            advance_status_revision(status_state);
            for (const ActiveStatusEffect& old_status : replaced) {
                engine.queue_status_effect_presentation(
                    apply_status->target,
                    old_status.status_effect_id,
                    old_status.instance_id,
                    KernelRemoteActionPresentationEventType_StatusRemoved,
                    old_status.stack_count);
            }
            engine.queue_status_effect_presentation(
                apply_status->target,
                active.status_effect_id,
                active.instance_id,
                KernelRemoteActionPresentationEventType_StatusApplied,
                active.stack_count);
            engine.publish_status_effect_state(apply_status->target);
            continue;
        }
        if (const auto* remove_status =
                std::get_if<ActionRemoveStatusCommand>(&command)) {
            const entt::entity target = *world.find_entity(remove_status->target);
            StatusEffectState* status_state =
                world.registry().try_get<StatusEffectState>(target);
            if (status_state == nullptr) {
                continue;
            }
            std::vector<ActiveStatusEffect> removed;
            for (const ActiveStatusEffect& active : status_state->active) {
                if (active.status_effect_id == remove_status->status_effect_id) {
                    removed.push_back(active);
                }
            }
            std::vector<PreparedStatusLifecycle> prepared_expire(removed.size());
            for (std::size_t removed_index = 0u;
                 removed_index < removed.size();
                 ++removed_index) {
                const ActiveStatusEffect& active = removed[removed_index];
                const RuntimeStatusEffectTemplate* status_template =
                    world.find_status_effect_template(active.status_effect_id);
                if (status_template != nullptr &&
                    !prepare_status_lifecycle_trigger(
                        engine,
                        remove_status->target,
                        active.source,
                        active.source_peer,
                        active.instance_id,
                        active.stack_count,
                        TriggerEventType::kStatusExpired,
                        status_template->on_expire_binding,
                        &prepared_expire[removed_index])) {
                    return false;
                }
            }
            if (removed.empty()) {
                continue;
            }
            for (const ActiveStatusEffect& active : removed) {
                status_state->speed_modifiers.erase(
                    std::remove_if(
                        status_state->speed_modifiers.begin(),
                        status_state->speed_modifiers.end(),
                        [&](const SpeedModifier& modifier) {
                            return modifier.status_instance_id == active.instance_id;
                        }),
                    status_state->speed_modifiers.end());
            }
            status_state->active.erase(
                std::remove_if(
                    status_state->active.begin(),
                    status_state->active.end(),
                    [&](const ActiveStatusEffect& active) {
                        return active.status_effect_id ==
                            remove_status->status_effect_id;
                    }),
                status_state->active.end());
            recompute_speed(world, target);
            for (PreparedStatusLifecycle& prepared : prepared_expire) {
                if (prepared.batch.has_value() &&
                    !execute_action_graph_commands(
                        engine,
                        world,
                        damage_pipeline,
                        entity_templates,
                        *prepared.batch,
                        server_time_us,
                        active_transaction)) {
                    return false;
                }
            }
            advance_status_revision(*status_state);
            for (const ActiveStatusEffect& active : removed) {
                engine.queue_status_effect_presentation(
                    remove_status->target,
                    active.status_effect_id,
                    active.instance_id,
                    KernelRemoteActionPresentationEventType_StatusRemoved,
                    active.stack_count);
            }
            engine.publish_status_effect_state(remove_status->target);
            continue;
        }
        if (const auto* modifier =
                std::get_if<ActionApplySpeedModifierCommand>(&command)) {
            const entt::entity target = *world.find_entity(modifier->target);
            if (!world.registry().all_of<EntityKind, MovementState>(target) ||
                world.registry().get<EntityKind>(target).type != EntityType::kActor) {
                continue;
            }
            StatusEffectState& status_state =
                world.registry().get_or_emplace<StatusEffectState>(target);
            if (std::none_of(
                    status_state.active.begin(),
                    status_state.active.end(),
                    [&](const ActiveStatusEffect& active) {
                        return active.instance_id == modifier->status_instance_id;
                    })) {
                return false;
            }
            auto found = std::find_if(
                status_state.speed_modifiers.begin(),
                status_state.speed_modifiers.end(),
                [&](const SpeedModifier& existing) {
                    return existing.status_instance_id == modifier->status_instance_id;
                });
            if (found == status_state.speed_modifiers.end()) {
                status_state.speed_modifiers.push_back(SpeedModifier{
                    modifier->status_instance_id,
                    modifier->operation == KernelStatModifierOperation_Additive
                        ? modifier->value : 0.0f,
                    modifier->operation == KernelStatModifierOperation_Multiplier
                        ? modifier->value : 1.0f,
                });
            } else if (modifier->operation == KernelStatModifierOperation_Additive) {
                found->additive = modifier->value;
                found->multiplier = 1.0f;
            } else {
                found->additive = 0.0f;
                found->multiplier = modifier->value;
            }
            recompute_speed(world, target);
            continue;
        }
        if (const auto* impulse =
                std::get_if<ActionApplyImpulseCommand>(&command)) {
            const entt::entity target = *world.find_entity(impulse->target);
            const glm::vec3 direction =
                glm::normalize(impulse->direction);
            const float resistance = world.registry().all_of<ImpulseResistance>(target)
                ? world.registry().get<ImpulseResistance>(target).value
                : 0.0f;
            if (!std::isfinite(resistance) || impulse->strength <= resistance) {
                continue;
            }
            Velocity& velocity = world.registry().get_or_emplace<Velocity>(target);
            velocity.linear += direction * impulse->strength;
            const EntityKind& kind = world.registry().get<EntityKind>(target);
            if (kind.type == EntityType::kActor) {
                MovementState& movement =
                    world.registry().get_or_emplace<MovementState>(target);
                movement.ground_state = MovementState::GroundState::kAirborne;
                movement.ground_normal = glm::vec3{0.0f, 1.0f, 0.0f};
                movement.supporting_entity_net_id = 0u;
                movement.supporting_collider_id = 0u;
                movement.has_controller_height = false;
            } else if (kind.type == EntityType::kProp) {
                PropWorldMode& mode =
                    world.registry().get_or_emplace<PropWorldMode>(target);
                mode.mode = PropMode::kInFlight;
                world.registry().erase<CarriedBy>(target);
                const Transform& transform = world.registry().get<Transform>(target);
                world.registry().emplace_or_replace<ThrownPropMotion>(
                    target,
                    ThrownPropMotion{
                        ProjectileMotionModel::kLinear,
                        0u,
                        transform.position,
                        velocity.linear,
                        glm::vec3{0.0f, -9.81f, 0.0f},
                        transform.position,
                    });
                if (world.registry().all_of<ItemInstanceRef>(target)) {
                    engine.item_store().set_world_mode(
                        world.registry().get<ItemInstanceRef>(target).item_instance_id,
                        KernelWorldItemMode_InFlight);
                }
                for (ColliderInstance& collider :
                     world.collider_registry().mutable_instances()) {
                    if (collider.entity_net_id == impulse->target) {
                        collider.enabled = true;
                        if (engine.mutable_physics_world() != nullptr) {
                            engine.mutable_physics_world()->set_object_enabled(
                                collider.collider_id, true);
                        }
                    }
                }
                engine.queue_prop_state_change(impulse->target);
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
        create_info.rotation = yaw_rotation_from_direction(spawn.direction);
        NetId spawned_net_id = 0;
        if (!EntityLifecycleSystem{}.create_entity(
                engine, create_info, &spawned_net_id, false)) {
            return false;
        }
        if (spawn.item_template_id != 0u) {
            const auto item_id = engine.item_store().create_world_item(
                spawn.item_template_id,
                spawn.quantity,
                spawned_net_id,
                KernelWorldItemMode_Placed);
            const std::optional<entt::entity> spawned =
                world.find_entity(spawned_net_id);
            if (!item_id.has_value() || !spawned.has_value()) {
                EntityLifecycleSystem{}.destroy_entity(
                    engine, spawned_net_id, KernelDespawnReason_Destroyed);
                return false;
            }
            world.registry().emplace_or_replace<ItemTemplateRef>(
                *spawned, ItemTemplateRef{spawn.item_template_id});
            world.registry().emplace_or_replace<ItemInstanceRef>(
                *spawned, ItemInstanceRef{*item_id});
            world.registry().emplace_or_replace<PropWorldMode>(
                *spawned, PropWorldMode{PropMode::kPlaced});
            const ItemInstanceRecord* item =
                engine.item_store().find_item(*item_id);
            if (item == nullptr ||
                !ItemGameplaySystem{}.decorate_item_prop(
                    engine, spawned_net_id, *item)) {
                EntityLifecycleSystem{}.destroy_entity(
                    engine, spawned_net_id, KernelDespawnReason_Destroyed);
                return false;
            }
            engine.queue_prop_state_change(spawned_net_id);
        }
    }
    if (transaction == nullptr &&
        !owned_transaction->commit(engine.current_tick())) {
        return false;
    }
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

std::uint32_t live_player_count(World& world) {
    std::uint32_t count = 0u;
    auto view = world.registry().view<const EntityKind>();
    for (const entt::entity entity : view) {
        const EntityKind& kind = view.get<const EntityKind>(entity);
        if (kind.type == EntityType::kActor &&
            kind.actor_type == ActorType::kPlayer) {
            ++count;
        }
    }
    return count;
}

GameRuleGroupRuntime* find_game_rule_group(
    GameRuleRuntime& runtime,
    std::uint32_t group_id) {
    const auto found = std::find_if(
        runtime.groups.begin(),
        runtime.groups.end(),
        [group_id](const GameRuleGroupRuntime& group) {
            return group.group_id == group_id;
        });
    return found == runtime.groups.end() ? nullptr : &*found;
}

const KernelGameRuleDefinition* find_game_rule_definition(
    const std::vector<KernelGameRuleDefinition>& definitions,
    std::uint32_t definition_id) {
    const auto found = std::find_if(
        definitions.begin(),
        definitions.end(),
        [definition_id](const KernelGameRuleDefinition& definition) {
            return definition.game_rule_definition_id == definition_id;
        });
    return found == definitions.end() ? nullptr : &*found;
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
    const DirectorKind kind = ai.director_kind == KernelDirectorKind_GameRule
        ? DirectorKind::kGameRule
        : DirectorKind::kWorldRule;
    registry.emplace_or_replace<DirectorRuntime>(
        entity,
        DirectorRuntime{
            kind,
            ai.tick_interval == 0u ? 1u : ai.tick_interval,
            0u,
        });
    if (kind == DirectorKind::kGameRule) {
        registry.emplace_or_replace<GameRuleRuntime>(
            entity,
            GameRuleRuntime{ai.game_rule_definition_id});
        return;
    }
    registry.emplace_or_replace<WorldRuleRuntime>(
        entity,
        WorldRuleRuntime{
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

bool execute_status_lifecycle_trigger(
    KernelEngine& engine,
    NetId target,
    NetId source,
    PeerId source_peer,
    std::uint32_t status_instance_id,
    std::uint16_t stack_count,
    TriggerEventType event_type,
    const std::optional<CompiledActionGraphBinding>& binding,
    std::uint64_t server_time_us) {
    PreparedStatusLifecycle prepared;
    if (!prepare_status_lifecycle_trigger(
            engine,
            target,
            source,
            source_peer,
            status_instance_id,
            stack_count,
            event_type,
            binding,
            &prepared)) {
        return false;
    }
    if (!prepared.batch.has_value()) {
        return true;
    }
    return execute_action_graph_commands(
        engine,
        engine.simulation_world(),
        &engine.damage_pipeline(),
        engine.authored_entity_templates(),
        *prepared.batch,
        server_time_us);
}

void simulate_status_effects(KernelEngine& engine, std::uint64_t server_time_us) {
    World& world = engine.simulation_world();
    std::vector<NetId> target_ids;
    const auto view = world.registry().view<NetworkIdentity, StatusEffectState>();
    target_ids.reserve(view.size_hint());
    for (const entt::entity entity : view) {
        target_ids.push_back(view.get<NetworkIdentity>(entity).net_id);
    }
    std::sort(target_ids.begin(), target_ids.end());
    const std::uint32_t current_tick = engine.current_tick();
    for (const NetId target_id : target_ids) {
        const std::optional<entt::entity> entity = world.find_entity(target_id);
        if (!entity.has_value()) {
            continue;
        }
        StatusEffectState& state = world.registry().get<StatusEffectState>(*entity);
        std::sort(
            state.active.begin(), state.active.end(),
            [](const ActiveStatusEffect& lhs, const ActiveStatusEffect& rhs) {
                return lhs.instance_id < rhs.instance_id;
            });
        bool active_set_changed = false;
        for (std::size_t index = 0u; index < state.active.size();) {
            const ActiveStatusEffect active = state.active[index];
            const RuntimeStatusEffectTemplate* status_template =
                world.find_status_effect_template(active.status_effect_id);
            if (status_template == nullptr) {
                state.active.erase(state.active.begin() + index);
                state.speed_modifiers.erase(
                    std::remove_if(
                        state.speed_modifiers.begin(),
                        state.speed_modifiers.end(),
                        [&](const SpeedModifier& modifier) {
                            return modifier.status_instance_id == active.instance_id;
                        }),
                    state.speed_modifiers.end());
                active_set_changed = true;
                engine.queue_status_effect_presentation(
                    target_id,
                    active.status_effect_id,
                    active.instance_id,
                    KernelRemoteActionPresentationEventType_StatusRemoved,
                    active.stack_count);
                continue;
            }
            if (current_tick >= active.expire_tick) {
                PreparedStatusLifecycle prepared_expire;
                if (!prepare_status_lifecycle_trigger(
                        engine,
                        target_id,
                        active.source,
                        active.source_peer,
                        active.instance_id,
                        active.stack_count,
                        TriggerEventType::kStatusExpired,
                        status_template->on_expire_binding,
                        &prepared_expire)) {
                    ++index;
                    continue;
                }
                state.speed_modifiers.erase(
                    std::remove_if(
                        state.speed_modifiers.begin(),
                        state.speed_modifiers.end(),
                        [&](const SpeedModifier& modifier) {
                            return modifier.status_instance_id == active.instance_id;
                        }),
                    state.speed_modifiers.end());
                state.active.erase(state.active.begin() + index);
                recompute_speed(world, *entity);
                if (prepared_expire.batch.has_value() &&
                    !execute_action_graph_commands(
                        engine,
                        world,
                        &engine.damage_pipeline(),
                        engine.authored_entity_templates(),
                        *prepared_expire.batch,
                        server_time_us)) {
                    return;
                }
                active_set_changed = true;
                engine.queue_status_effect_presentation(
                    target_id,
                    active.status_effect_id,
                    active.instance_id,
                    KernelRemoteActionPresentationEventType_StatusRemoved,
                    active.stack_count);
                continue;
            }
            if (status_template->interval_ticks != 0u &&
                current_tick >= active.next_tick) {
                if (!execute_status_lifecycle_trigger(
                        engine,
                        target_id,
                        active.source,
                        active.source_peer,
                        active.instance_id,
                        active.stack_count,
                        TriggerEventType::kStatusTick,
                        status_template->on_tick_binding,
                        server_time_us)) {
                    return;
                }
                state.active[index].next_tick =
                    active.next_tick + status_template->interval_ticks;
            }
            ++index;
        }
        recompute_speed(world, *entity);
        if (active_set_changed) {
            advance_status_revision(state);
            engine.publish_status_effect_state(target_id);
        }
    }
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
            // A deployable's side is whoever deployed it, not its template: the
            // same ice block is cover for a player and cover for an agent.
            // owner_peer survives every hop of a spawn chain -- a thrown bottle
            // hands its own to the block it spawns on impact -- and is zero for
            // exactly the server-driven agents, which have no peer.
            registry.emplace_or_replace<GameplaySide>(
                *entity,
                GameplaySide{
                    create_info.owner_peer != 0u
                        ? kCollisionLayerPlayerSide
                        : kCollisionLayerHostileSide});
            if (entity_template->prop.lifetime_ticks != 0u ||
                entity_template->prop.population_group_id != 0u) {
                registry.emplace_or_replace<PropLifecycle>(
                    *entity,
                    PropLifecycle{
                        engine.tick_loop_.current_tick(),
                        entity_template->prop.lifetime_ticks,
                        entity_template->prop.population_group_id,
                    });
            }
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
        registry.emplace_or_replace<ImpulseResistance>(
            *entity,
            ImpulseResistance{entity_template->impulse_resistance});
        if ((entity_template->component_flags & KERNEL_ENTITY_COMPONENT_HEALTH) !=
            0u) {
            registry.emplace_or_replace<Health>(
                *entity,
                Health{
                    entity_template->combat.hp,
                    entity_template->combat.max_hp,
                });
            MovementState& movement = registry.get_or_emplace<MovementState>(*entity);
            movement.base_speed_meters_per_second =
                entity_template->combat.move_speed_meters_per_second;
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
            movement.movement_collision_mask =
                entity_template->movement.movement_collision_mask;
            movement.locomotion_owns_height =
                entity_template->skeleton.body_follow_speed > 0.0f;
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
                ActionGraphCollisionBinding{
                    *binding,
                    entity_template->collision_trigger_mask});
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
    if (entity_template != nullptr &&
        entity_template->prop.population_group_id != 0u) {
        enforce_prop_population_limit(
            engine, entity_template->prop.population_group_id);
    }
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
        PropWorldMode,
        Velocity,
        OnCollisionTriggerTag,
        ActionGraphCollisionBinding>();
    for (const entt::entity entity : view) {
        const NetworkIdentity& identity = view.get<NetworkIdentity>(entity);
        const EntityKind& kind = view.get<EntityKind>(entity);
        if (kind.type != EntityType::kProp) {
            continue;
        }
        PropWorldMode& mode = view.get<PropWorldMode>(entity);
        if (mode.mode != PropMode::kInFlight) {
            continue;
        }
        const ActionGraphCollisionBinding& collision_binding =
            view.get<ActionGraphCollisionBinding>(entity);
        glm::vec3 collision_direction =
            normalized_direction_or_zero(view.get<Velocity>(entity).linear);
        if (const ThrownPropMotion* motion =
                engine.world_.registry().try_get<ThrownPropMotion>(entity)) {
            const Transform& transform =
                engine.world_.registry().get<Transform>(entity);
            const glm::vec3 swept_direction = normalized_direction_or_zero(
                transform.position - motion->previous_position);
            if (swept_direction != glm::vec3{0.0f}) {
                collision_direction = swept_direction;
            }
        }
        std::unordered_set<NetId> seen_targets;
        std::optional<std::pair<physics::CollisionHit, std::uint32_t>>
            static_contact;
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
            physics::CollisionQueryFilter filter = collision_filter_from_mask(
                collision_binding.collision_mask);
            filter.ignored_entity_net_id = identity.net_id;
            std::vector<physics::CollisionHit> hits;
            const ThrownPropMotion* thrown_motion =
                engine.world_.registry().try_get<ThrownPropMotion>(entity);
            if (thrown_motion != nullptr) {
                const Transform& transform =
                    engine.world_.registry().get<Transform>(entity);
                const glm::vec3 previous_center =
                    thrown_motion->previous_position +
                    transform.rotation * collider.local_center;
                const glm::vec3 displacement =
                    collider.world_center - previous_center;
                if (glm::dot(displacement, displacement) > 0.00000001f) {
                    physics::ShapeCastRequest request{};
                    request.shape.type =
                        collider.shape_type == ColliderShapeType::kSphere
                        ? physics::CollisionShapeType::kSphere
                        : collider.shape_type == ColliderShapeType::kCapsule
                            ? physics::CollisionShapeType::kCapsule
                            : physics::CollisionShapeType::kBox;
                    request.shape.half_extents = collider.half_extents;
                    request.shape.radius = collider.radius;
                    request.shape.capsule_half_height =
                        collider.capsule_half_height;
                    request.start = previous_center;
                    request.rotation = collider.world_rotation;
                    request.displacement = displacement;
                    request.filter = filter;
                    hits = engine.physics_world_->shape_cast_all(request);
                }
            }
            if (hits.empty()) {
                physics::OverlapRequest request{};
                request.shape.type =
                    collider.shape_type == ColliderShapeType::kSphere
                    ? physics::CollisionShapeType::kSphere
                    : collider.shape_type == ColliderShapeType::kCapsule
                        ? physics::CollisionShapeType::kCapsule
                        : physics::CollisionShapeType::kBox;
                request.shape.half_extents = collider.half_extents;
                request.shape.radius = collider.radius;
                request.shape.capsule_half_height =
                    collider.capsule_half_height;
                request.position = collider.world_center;
                request.rotation = collider.world_rotation;
                request.filter = filter;
                hits = engine.physics_world_->overlap_all(request);
                for (physics::CollisionHit& hit : hits) {
                    hit.fraction = 1.0f;
                }
            }
            for (const physics::CollisionHit& hit : hits) {
                if (hit.identity.kind !=
                        physics::CollisionObjectKind::kActorHitbox) {
                    const auto hit_key = std::tuple{
                        hit.fraction,
                        hit.distance,
                        collider.collider_id,
                        static_cast<std::uint8_t>(hit.identity.kind),
                        hit.identity.collider_id,
                        hit.subshape_id,
                    };
                    if (!static_contact.has_value()) {
                        static_contact = std::pair{hit, collider.collider_id};
                    } else {
                        const physics::CollisionHit& current =
                            static_contact->first;
                        const auto current_key = std::tuple{
                            current.fraction,
                            current.distance,
                            static_contact->second,
                            static_cast<std::uint8_t>(current.identity.kind),
                            current.identity.collider_id,
                            current.subshape_id,
                        };
                        if (hit_key < current_key) {
                            static_contact =
                                std::pair{hit, collider.collider_id};
                        }
                    }
                    continue;
                }
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
                        collision_direction == glm::vec3{0.0f}
                            ? hit.normal
                            : collision_direction,
                        collision_binding.binding,
                    });
                }
            }
        }
        if (static_contact.has_value()) {
            if (const ThrownPropMotion* motion =
                    engine.world_.registry().try_get<ThrownPropMotion>(entity)) {
                Transform& transform =
                    engine.world_.registry().get<Transform>(entity);
                const glm::vec3 displacement =
                    transform.position - motion->previous_position;
                transform.position = motion->previous_position +
                    displacement *
                        std::clamp(static_contact->first.fraction, 0.0f, 1.0f);
                engine.world_.registry().remove<ThrownPropMotion>(entity);
            }
            mode.mode = PropMode::kPlaced;
            view.get<Velocity>(entity).linear = glm::vec3{0.0f};
            if (engine.world_.registry().all_of<ItemInstanceRef>(entity)) {
                const ItemInstanceRef& ref =
                    engine.world_.registry().get<ItemInstanceRef>(entity);
                engine.item_store_.set_world_mode(
                    ref.item_instance_id,
                    KernelWorldItemMode_Placed);
            }
            engine.queue_prop_state_change(identity.net_id);
            entered_collisions.push_back(CollisionFact{
                identity.net_id,
                0u,
                identity.owner_peer,
                static_contact->first.position,
                collision_direction == glm::vec3{0.0f}
                    ? static_contact->first.normal
                    : collision_direction,
                collision_binding.binding,
            });
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

void EntityLifecycleSystem::update_prop_lifetimes(
    KernelEngine& engine) const {
    std::vector<NetId> expired;
    auto view = engine.world_.registry().view<NetworkIdentity, PropLifecycle>();
    for (const entt::entity entity : view) {
        PropLifecycle& lifecycle = view.get<PropLifecycle>(entity);
        if (lifecycle.remaining_lifetime_ticks == 0u) {
            continue;
        }
        --lifecycle.remaining_lifetime_ticks;
        if (lifecycle.remaining_lifetime_ticks == 0u) {
            expired.push_back(view.get<NetworkIdentity>(entity).net_id);
        }
    }
    std::sort(expired.begin(), expired.end());
    for (const NetId net_id : expired) {
        // Lifecycle expiry is resource cleanup and intentionally bypasses
        // gameplay on_destroy_entity graphs.
        (void)destroy_entity_with_context(
            engine,
            net_id,
            KernelDespawnReason_Expired,
            0u,
            0u,
            nullptr,
            false);
    }
}

void EntityLifecycleSystem::enforce_prop_population_limit(
    KernelEngine& engine,
    std::uint32_t population_group_id) const {
    const auto rule = std::find_if(
        engine.prop_population_rules_.begin(),
        engine.prop_population_rules_.end(),
        [population_group_id](
            const KernelPropPopulationRuleDefinition& candidate) {
            return candidate.population_group_id == population_group_id;
        });
    if (rule == engine.prop_population_rules_.end()) {
        return;
    }
    std::vector<std::tuple<std::uint32_t, NetId>> members;
    auto view = engine.world_.registry().view<NetworkIdentity, PropLifecycle>();
    for (const entt::entity entity : view) {
        const PropLifecycle& lifecycle = view.get<PropLifecycle>(entity);
        if (lifecycle.population_group_id != population_group_id) {
            continue;
        }
        members.emplace_back(
            lifecycle.spawn_tick,
            view.get<NetworkIdentity>(entity).net_id);
    }
    std::sort(members.begin(), members.end());
    // V1 fixes overflow handling to deterministic despawn-oldest. Add an
    // authored overflow policy before supporting alternatives such as reject-new.
    while (members.size() > rule->max_alive) {
        const NetId oldest = std::get<1>(members.front());
        members.erase(members.begin());
        // Capacity eviction is resource cleanup and intentionally bypasses
        // gameplay on_destroy_entity graphs to prevent spawn cascades.
        (void)destroy_entity_with_context(
            engine,
            oldest,
            KernelDespawnReason_CapacityEvicted,
            0u,
            0u,
            nullptr,
            false);
    }
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
    const glm::vec3* event_position,
    bool execute_destroy_graph) const {
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
    KernelItemInstanceId world_item_id = 0u;
    if (engine.world_.registry().all_of<ItemInstanceRef>(*entity)) {
        const KernelItemInstanceId candidate =
            engine.world_.registry().get<ItemInstanceRef>(*entity)
                .item_instance_id;
        const ItemInstanceRecord* item = engine.item_store_.find_item(candidate);
        if (item != nullptr && !item->terminal &&
            item->residency.kind == KernelItemResidency_World &&
            item->residency.prop_entity_id == net_id) {
            world_item_id = candidate;
        }
    }
    std::optional<GameplayGroupMembership> group_membership;
    if (engine.world_.registry().all_of<GameplayGroupMembership>(*entity)) {
        group_membership =
            engine.world_.registry().get<GameplayGroupMembership>(*entity);
    }
    std::vector<ActionGraphQueuedTrigger> queued_triggers;
    if (execute_destroy_graph &&
        engine.world_.registry().all_of<
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
    if (group_membership.has_value()) {
        const std::optional<entt::entity> director =
            engine.world_.find_entity(group_membership->director_net_id);
        if (director.has_value() &&
            engine.world_.registry().all_of<GameRuleRuntime>(*director)) {
            GameRuleRuntime& runtime =
                engine.world_.registry().get<GameRuleRuntime>(*director);
            GameRuleGroupRuntime* group = find_game_rule_group(
                runtime, group_membership->group_id);
            if (group != nullptr && group->alive_count > 0u) {
                --group->alive_count;
            }
        }
    }
    if (world_item_id != 0u) {
        (void)engine.item_store_.terminate(world_item_id);
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
        movement.movement_collision_mask =
            authored_entity_template->movement.movement_collision_mask;
        movement.locomotion_owns_height =
            authored_entity_template->skeleton.body_follow_speed > 0.0f;
        movement.ground_state = MovementState::GroundState::kAirborne;
        movement.has_last_queried_position = false;
        // Respawn moves the body without the controller; its remembered anchor
        // belongs to the old position.
        movement.has_controller_height = false;
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
    // An authoritative teleport is the one mover the character controller does
    // not perform itself, so its remembered vertical anchor now describes the
    // position the entity just left. Drop it and let the next step re-resolve.
    if (engine.world_.registry().all_of<MovementState>(*entity)) {
        engine.world_.registry().get<MovementState>(*entity)
            .has_controller_height = false;
    }
    engine.sync_entity_colliders_from_world();
    if (engine.world_.registry().all_of<EntityKind>(*entity) &&
        engine.world_.registry().get<EntityKind>(*entity).type == EntityType::kProp) {
        engine.queue_prop_state_change(net_id);
    }
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
    if (engine.world_.registry().all_of<EntityKind>(*entity) &&
        engine.world_.registry().get<EntityKind>(*entity).type == EntityType::kProp) {
        engine.queue_prop_state_change(net_id);
    }
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

bool MovementSystem::submit_player_input(
    KernelEngine& engine,
    NetId net_id,
    const KernelPlayerInput& input) const {
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
    if (intent.scope != ai::IntentScope::kDirector) {
        result.status = ai::IntentStatus::kFailed;
        result.unsupported = true;
        return result;
    }
    if (intent.type == "SpawnGroup") {
        const std::optional<std::uint32_t> node_id =
            uint32_param(intent, "node_id");
        const std::optional<std::uint32_t> group_id =
            uint32_param(intent, "group_id");
        const std::optional<std::uint32_t> spawn_count =
            uint32_param(intent, "count");
        const std::optional<std::uint32_t> entity_template_id =
            uint32_param(intent, "entity_template_id");
        const std::optional<float> position_x =
            float_param(intent, "position_x");
        const std::optional<float> position_y =
            float_param(intent, "position_y");
        const std::optional<float> position_z =
            float_param(intent, "position_z");
        const std::optional<float> radius = float_param(intent, "radius");
        const std::optional<std::uint32_t> seed = uint32_param(intent, "seed");
        if (!node_id.has_value() || !group_id.has_value() ||
            !spawn_count.has_value() || !entity_template_id.has_value() ||
            !position_x.has_value() || !position_y.has_value() ||
            !position_z.has_value() || !radius.has_value() || !seed.has_value() ||
            *spawn_count == 0u || intent.subject == 0u || !engine.running_ ||
            !is_server_mode(engine.config_.mode)) {
            return result;
        }
        const std::optional<entt::entity> entity =
            engine.world_.find_entity(intent.subject);
        if (!entity.has_value() ||
            !engine.world_.registry().all_of<
                DirectorRuntime,
                GameRuleRuntime,
                ServerOnly,
                EntityKind>(*entity)) {
            return result;
        }
        DirectorRuntime& director =
            engine.world_.registry().get<DirectorRuntime>(*entity);
        GameRuleRuntime& runtime =
            engine.world_.registry().get<GameRuleRuntime>(*entity);
        GameRuleGroupRuntime* group = find_game_rule_group(runtime, *group_id);
        const KernelGameRuleDefinition* definition = find_game_rule_definition(
            engine.game_rule_definitions_, runtime.definition_id);
        if (director.kind != DirectorKind::kGameRule || definition == nullptr ||
            runtime.status != GameRuleStatus::kRunning || group == nullptr ||
            group->pending_spawn_count != 0u || group->sealed) {
            return result;
        }
        const auto effect = std::find_if(
            engine.game_rule_effects_.begin() + definition->first_effect,
            engine.game_rule_effects_.begin() + definition->first_effect +
                definition->effect_count,
            [&](const KernelGameRuleSpawnGroupEffectDefinition& candidate) {
                return candidate.node_id == *node_id &&
                    candidate.group_id == *group_id &&
                    candidate.count == *spawn_count &&
                    candidate.entity_template_id == *entity_template_id;
            });
        if (effect == engine.game_rule_effects_.begin() +
                          definition->first_effect + definition->effect_count ||
            effect->position.x != *position_x || effect->position.y != *position_y ||
            effect->position.z != *position_z || effect->radius != *radius ||
            effect->seed != *seed) {
            return result;
        }
        group->pending_spawn_count = *spawn_count;
        for (std::uint32_t index = 0u; index < *spawn_count; ++index) {
            const float angle =
                static_cast<float>(*seed + index) * 2.39996323f;
            KernelServerEntityCreateInfo create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.owner_peer = 0u;
            create_info.entity_template_id = *entity_template_id;
            create_info.position = KernelVec3{
                *position_x + std::cos(angle) * *radius,
                *position_y,
                *position_z + std::sin(angle) * *radius,
            };
            create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
            simulation::Command command{};
            command.id = simulation::CommandId::kCreateEntity;
            command.source = simulation::CommandSource::kAi;
            command.create_entity.create_info = create_info;
            command.create_entity.director_net_id = intent.subject;
            command.create_entity.gameplay_group_id = *group_id;
            command.create_entity.spawn_batch_id = *group_id;
            if (!engine.enqueue_simulation_command(command)) {
                group->failed = true;
                runtime.status = GameRuleStatus::kFailed;
                result.status = ai::IntentStatus::kFailed;
                return result;
            }
            ++result.created_count;
        }
        result.status = ai::IntentStatus::kSucceeded;
        return result;
    }
    if (intent.type != "SpawnAgent") {
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
            WorldRuleRuntime,
            Transform,
            ServerOnly,
            EntityKind>(*entity)) {
        return result;
    }

    auto& registry = engine.world_.registry();
    const EntityKind& kind = registry.get<EntityKind>(*entity);
    const AgentRuntime& agent = registry.get<AgentRuntime>(*entity);
    DirectorRuntime& director_common = registry.get<DirectorRuntime>(*entity);
    WorldRuleRuntime& director = registry.get<WorldRuleRuntime>(*entity);
    if (kind.type != EntityType::kDirector ||
        agent.controller_type != AiControllerType::kDirector ||
        director_common.kind != DirectorKind::kWorldRule ||
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
                director_common.next_tick =
                    engine.tick_loop_.current_tick() +
                    std::max<std::uint32_t>(1u, director_common.tick_interval);
            }
            result.status = ai::IntentStatus::kFailed;
            return result;
        }
        ++result.created_count;
    }
    director.spawn_cursor += result.created_count;
    director_common.next_tick =
        engine.tick_loop_.current_tick() +
        std::max<std::uint32_t>(1u, director_common.tick_interval);
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

    auto world_rule_view =
        engine.world_.registry()
            .view<
                EntityKind,
                NetworkIdentity,
                AgentRuntime,
                DirectorRuntime,
                WorldRuleRuntime,
                Transform,
                ServerOnly>();
    for (const entt::entity entity : world_rule_view) {
        const EntityKind& kind = world_rule_view.get<EntityKind>(entity);
        const NetworkIdentity& identity =
            world_rule_view.get<NetworkIdentity>(entity);
        AgentRuntime& agent = world_rule_view.get<AgentRuntime>(entity);
        DirectorRuntime& director = world_rule_view.get<DirectorRuntime>(entity);
        WorldRuleRuntime& world_rule =
            world_rule_view.get<WorldRuleRuntime>(entity);
        if (kind.type != EntityType::kDirector ||
            agent.controller_type != AiControllerType::kDirector ||
            director.kind != DirectorKind::kWorldRule ||
            world_rule.spawn_target_count <= live_count ||
            engine.tick_loop_.current_tick() < director.next_tick) {
            continue;
        }

        const std::uint32_t missing_count =
            world_rule.spawn_target_count - live_count;
        ai::ScopedIntent intent;
        intent.scope = ai::IntentScope::kDirector;
        intent.type = "SpawnAgent";
        intent.subject = identity.net_id;
        intent.params["count"] = missing_count;
        intent.params["spawn_target_count"] = world_rule.spawn_target_count;
        intent.params["spawn_entity_template_id"] =
            world_rule.spawn_entity_template_id;
        intent.params["spawn_actor_template_id"] =
            world_rule.spawn_actor_template_id;
        intent.params["spawn_position_x"] = world_rule.spawn_position.x;
        intent.params["spawn_position_y"] = world_rule.spawn_position.y;
        intent.params["spawn_position_z"] = world_rule.spawn_position.z;
        intent.params["spawn_radius"] = world_rule.spawn_radius;
        intent.params["base_spawn_cursor"] = world_rule.spawn_cursor;
        engine.pending_director_intents_.push_back(std::move(intent));
        live_count += missing_count;
    }

    auto game_rule_view =
        engine.world_.registry()
            .view<
                EntityKind,
                NetworkIdentity,
                DirectorRuntime,
                GameRuleRuntime,
                ServerOnly>();
    for (const entt::entity entity : game_rule_view) {
        const EntityKind& kind = game_rule_view.get<EntityKind>(entity);
        const NetworkIdentity& identity =
            game_rule_view.get<NetworkIdentity>(entity);
        DirectorRuntime& director = game_rule_view.get<DirectorRuntime>(entity);
        GameRuleRuntime& runtime = game_rule_view.get<GameRuleRuntime>(entity);
        if (kind.type != EntityType::kDirector ||
            director.kind != DirectorKind::kGameRule ||
            runtime.status != GameRuleStatus::kRunning ||
            engine.tick_loop_.current_tick() < director.next_tick) {
            continue;
        }
        const KernelGameRuleDefinition* definition = find_game_rule_definition(
            engine.game_rule_definitions_, runtime.definition_id);
        if (definition == nullptr) {
            runtime.status = GameRuleStatus::kFailed;
            continue;
        }
        const auto node_begin =
            engine.game_rule_nodes_.begin() + definition->first_node;
        const auto edge_begin =
            engine.game_rule_edges_.begin() + definition->first_edge;
        const auto effect_begin =
            engine.game_rule_effects_.begin() + definition->first_effect;
        const std::uint32_t player_count = live_player_count(engine.world_);
        const auto node_offset = [&](std::uint32_t node_id)
            -> std::optional<std::size_t> {
            const auto found = std::find_if(
                node_begin,
                node_begin + definition->node_count,
                [node_id](const KernelGameRuleNodeDefinition& node) {
                    return node.node_id == node_id;
                });
            if (found == node_begin + definition->node_count) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(found - node_begin);
        };
        const auto activate = [&](std::size_t offset) {
            runtime.node_states[offset] = GameRuleNodeState::kActive;
            const KernelGameRuleNodeDefinition& node = node_begin[offset];
            const auto effect = std::find_if(
                effect_begin,
                effect_begin + definition->effect_count,
                [&](const KernelGameRuleSpawnGroupEffectDefinition& candidate) {
                    return candidate.node_id == node.node_id;
                });
            if (effect == effect_begin + definition->effect_count) {
                if (node.condition_type !=
                    KernelGameRuleConditionType_PlayerCountAtLeast) {
                    runtime.status = GameRuleStatus::kFailed;
                }
                return;
            }
            ai::ScopedIntent intent;
            intent.scope = ai::IntentScope::kDirector;
            intent.type = "SpawnGroup";
            intent.subject = identity.net_id;
            intent.params["node_id"] = effect->node_id;
            intent.params["group_id"] = effect->group_id;
            intent.params["count"] = effect->count;
            intent.params["entity_template_id"] = effect->entity_template_id;
            intent.params["position_x"] = effect->position.x;
            intent.params["position_y"] = effect->position.y;
            intent.params["position_z"] = effect->position.z;
            intent.params["radius"] = effect->radius;
            intent.params["seed"] = effect->seed;
            engine.pending_director_intents_.push_back(std::move(intent));
        };
        if (!runtime.initialized) {
            runtime.node_states.assign(
                definition->node_count, GameRuleNodeState::kInactive);
            runtime.groups.clear();
            runtime.groups.reserve(definition->node_count);
            for (std::uint32_t offset = 0u; offset < definition->node_count;
                 ++offset) {
                if (node_begin[offset].condition_type ==
                    KernelGameRuleConditionType_GroupEliminated) {
                    runtime.groups.push_back(GameRuleGroupRuntime{
                        node_begin[offset].condition_group_id});
                }
            }
            runtime.initialized = true;
            for (std::uint32_t offset = 0u; offset < definition->node_count;
                 ++offset) {
                const std::uint32_t node_id = node_begin[offset].node_id;
                const bool has_predecessor = std::any_of(
                    edge_begin,
                    edge_begin + definition->edge_count,
                    [node_id](const KernelGameRuleEdgeDefinition& edge) {
                        return edge.target_node_id == node_id;
                    });
                if (!has_predecessor) {
                    activate(offset);
                }
            }
        } else {
            for (std::uint32_t offset = 0u; offset < definition->node_count;
                 ++offset) {
                if (runtime.node_states[offset] != GameRuleNodeState::kActive) {
                    continue;
                }
                const KernelGameRuleNodeDefinition& node = node_begin[offset];
                bool completed = false;
                if (node.condition_type ==
                    KernelGameRuleConditionType_GroupEliminated) {
                    GameRuleGroupRuntime* group = find_game_rule_group(
                        runtime, node.condition_group_id);
                    completed = group != nullptr && group->sealed &&
                        !group->failed && group->alive_count == 0u;
                } else if (node.condition_type ==
                           KernelGameRuleConditionType_PlayerCountAtLeast) {
                    completed = player_count >= node.condition_count;
                }
                if (completed) {
                    runtime.node_states[offset] = GameRuleNodeState::kCompleted;
                }
            }
            for (std::uint32_t offset = 0u; offset < definition->node_count;
                 ++offset) {
                if (runtime.node_states[offset] != GameRuleNodeState::kInactive) {
                    continue;
                }
                const std::uint32_t node_id = node_begin[offset].node_id;
                bool has_predecessor = false;
                bool all_completed = true;
                for (std::uint32_t edge_offset = 0u;
                     edge_offset < definition->edge_count;
                     ++edge_offset) {
                    const KernelGameRuleEdgeDefinition& edge =
                        edge_begin[edge_offset];
                    if (edge.target_node_id != node_id) {
                        continue;
                    }
                    has_predecessor = true;
                    const std::optional<std::size_t> predecessor =
                        node_offset(edge.source_node_id);
                    if (!predecessor.has_value() ||
                        runtime.node_states[*predecessor] !=
                            GameRuleNodeState::kCompleted) {
                        all_completed = false;
                    }
                }
                if (has_predecessor && all_completed) {
                    activate(offset);
                }
            }
        }
        if (runtime.status == GameRuleStatus::kRunning &&
            std::all_of(
                runtime.node_states.begin(),
                runtime.node_states.end(),
                [](GameRuleNodeState state) {
                    return state == GameRuleNodeState::kCompleted;
                })) {
            runtime.status = GameRuleStatus::kCompleted;
        }
        director.next_tick = engine.tick_loop_.current_tick() +
            std::max<std::uint32_t>(1u, director.tick_interval);
    }
}

}  // namespace network_example
