#include "simulation/public/action_graph.h"

#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace network_example {
namespace {

// Tells one iteration of one repeated action apart from every other launch
// under the same parent salt.
std::uint32_t repeat_salt(
    std::uint32_t parent_salt,
    std::uint32_t action_index,
    std::uint32_t iteration) {
    return static_cast<std::uint32_t>(
        projectile_launch_seed(parent_salt, action_index, iteration, 0x52455054u));
}

// A repeated spawn_projectile, unrolled into its commands. Everything picked
// here is a pure function of the provenance and the iteration -- the count
// and the stagger in integers -- so the same event yields the same spread on
// any machine that runs this.
void append_repeated_spawns(
    const ActionSpawnProjectileDefinition& spawn,
    std::uint32_t projectile_template_id,
    const glm::vec3& position,
    const glm::vec3& direction,
    const ActionExecutionProvenance& provenance,
    std::vector<ActionGraphCommand>* commands) {
    constexpr std::uint32_t kCountSlot = 0xFFFFFFFFu;
    const std::uint32_t span =
        spawn.repeat_count_max - spawn.repeat_count_min + 1u;
    const std::uint64_t count_seed = projectile_launch_seed(
        provenance.instigator,
        provenance.action_instance_id,
        projectile_template_id,
        repeat_salt(provenance.launch_salt, spawn.action_index, kCountSlot));
    const std::uint32_t count = std::min<std::uint32_t>(
        spawn.repeat_count_min + static_cast<std::uint32_t>(count_seed % span),
        KERNEL_MAX_ACTION_REPEAT);
    const std::uint32_t window = spawn.repeat_stagger_lifetime_ticks;
    for (std::uint32_t iteration = 0; iteration < count; ++iteration) {
        ActionExecutionProvenance iteration_provenance = provenance;
        iteration_provenance.launch_salt =
            repeat_salt(provenance.launch_salt, spawn.action_index, iteration);
        const std::uint64_t seed = projectile_launch_seed(
            provenance.instigator,
            provenance.action_instance_id,
            projectile_template_id,
            iteration_provenance.launch_salt);
        // Uniform over the disc: the radius goes as the square root of a
        // uniform draw. Two separate 24-bit fields of the seed.
        const float radial =
            static_cast<float>(seed >> 40) * (1.0f / 16777216.0f);
        const float turn =
            static_cast<float>((seed >> 16) & 0xFFFFFFu) * (1.0f / 16777216.0f);
        const float radius = spawn.repeat_scatter_radius * std::sqrt(radial);
        const float angle = turn * 6.28318530717958647692f;
        ActionSpawnProjectileCommand command{
            projectile_template_id,
            position + glm::vec3{
                radius * std::cos(angle), 0.0f, radius * std::sin(angle)},
            direction,
            iteration_provenance};
        command.lifetime_ticks = spawn.lifetime_ticks;
        // Stratified: iteration i lands in the i-th of `count` equal slices of
        // the window, somewhere inside it. The expiries therefore come in
        // iteration order and never bunch up.
        if (window > 0u) {
            command.extra_lifetime_ticks = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(iteration) * window +
                 seed % window) /
                count);
        }
        commands->push_back(command);
    }
}


enum class ParameterType : std::uint8_t {
    kUnknown,
    kEntityId,
    kProjectileTemplateId,
    kEntityTemplateId,
    kStatusEffectId,
    kItemInstanceId,
    kVec3,
    kNumber,
};

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool is_status_lifecycle_event(TriggerEventType event_type) {
    return event_type == TriggerEventType::kStatusApplied ||
        event_type == TriggerEventType::kStatusTick ||
        event_type == TriggerEventType::kStatusExpired;
}

NetId action_source(NetId self, const TriggerEvent& event) {
    if (is_status_lifecycle_event(event.type)) {
        return event.instigator;
    }
    return event.type == TriggerEventType::kItemUsed && self == 0u
        ? event.instigator
        : self;
}

ParameterType value_type(const ActionGraphParameterValue& value) {
    if (std::holds_alternative<EntityIdValue>(value)) {
        return ParameterType::kEntityId;
    }
    if (std::holds_alternative<ProjectileTemplateIdValue>(value)) {
        return ParameterType::kProjectileTemplateId;
    }
    if (std::holds_alternative<EntityTemplateIdValue>(value)) {
        return ParameterType::kEntityTemplateId;
    }
    if (std::holds_alternative<StatusEffectIdValue>(value)) {
        return ParameterType::kStatusEffectId;
    }
    if (std::holds_alternative<ItemInstanceIdValue>(value)) {
        return ParameterType::kItemInstanceId;
    }
    if (std::holds_alternative<glm::vec3>(value)) {
        return ParameterType::kVec3;
    }
    if (std::holds_alternative<float>(value)) {
        return ParameterType::kNumber;
    }
    return ParameterType::kUnknown;
}

ParameterType expression_type(const ActionGraphParameterExpression& expression) {
    if (const auto* value =
            std::get_if<ActionGraphParameterValue>(&expression)) {
        return value_type(*value);
    }
    if (std::holds_alternative<EntityRefExpression>(expression)) {
        return ParameterType::kEntityId;
    }
    if (std::holds_alternative<ItemRefExpression>(expression)) {
        return ParameterType::kItemInstanceId;
    }
    return ParameterType::kVec3;
}

bool expression_available_for_event(
    const ActionGraphParameterExpression& expression,
    TriggerEventType event_type) {
    if (const auto* entity_ref =
            std::get_if<EntityRefExpression>(&expression)) {
        if (entity_ref->source == EntityRefSource::kEventTarget) {
            return event_type == TriggerEventType::kActivated ||
                event_type == TriggerEventType::kItemUsed ||
                event_type == TriggerEventType::kCollision ||
                event_type == TriggerEventType::kProjectileImpact ||
                event_type == TriggerEventType::kStatusApplied ||
                event_type == TriggerEventType::kStatusTick ||
                event_type == TriggerEventType::kStatusExpired;
        }
        if (entity_ref->source == EntityRefSource::kEventInstigator) {
            return event_type != TriggerEventType::kCollision;
        }
        return true;
    }
    const auto* event_vec3 = std::get_if<EventVec3Expression>(&expression);
    if (std::holds_alternative<ItemRefExpression>(expression)) {
        return event_type == TriggerEventType::kItemUsed;
    }
    if (event_vec3 == nullptr ||
        event_vec3->source == EventVec3Source::kPosition) {
        return true;
    }
    if (event_vec3->source == EventVec3Source::kSubjectPosition) {
        // Filled in by the projectile, area-effect and melee producers only;
        // anywhere else it would be a zero vector, which reads as the world
        // origin and would pull a target across the map.
        return event_type == TriggerEventType::kProjectileImpact ||
            event_type == TriggerEventType::kExpired;
    }
    if (event_vec3->source == EventVec3Source::kSubjectDirection) {
        // Only the projectile triggers carry a subject that was going
        // somewhere. The others have a subject that is standing still, or no
        // single heading to report, and answering them with a zero vector
        // would be worse than refusing.
        return event_type == TriggerEventType::kProjectileImpact ||
            event_type == TriggerEventType::kExpired;
    }
    // A destroy reports away from whoever did it, or straight up when nobody
    // did, so it always has a direction to give.
    return event_type == TriggerEventType::kActivated ||
        event_type == TriggerEventType::kItemUsed ||
        event_type == TriggerEventType::kCollision ||
        event_type == TriggerEventType::kProjectileImpact ||
        event_type == TriggerEventType::kExpired ||
        event_type == TriggerEventType::kDestroyEntity;
}

// Deliberately a switch rather than the cast this used to be. The two enums are
// separate types that only happened to line up, and KernelEventVec3Source_Literal
// has no counterpart here at all -- callers strip that one before asking, since
// a literal resolves to a value rather than to an event lookup. Anything else is
// a source this build does not implement, and the position is the one every
// trigger provides.
EventVec3Source event_vec3_source_from_kernel(std::uint8_t source) {
    switch (source) {
        case KernelEventVec3Source_Direction:
            return EventVec3Source::kDirection;
        case KernelEventVec3Source_SubjectDirection:
            return EventVec3Source::kSubjectDirection;
        case KernelEventVec3Source_SubjectPosition:
            return EventVec3Source::kSubjectPosition;
        case KernelEventVec3Source_Position:
        default:
            return EventVec3Source::kPosition;
    }
}

std::optional<ActionConditionType> action_condition_from_kernel(
    std::uint32_t condition_type) {
    if (condition_type == KernelActionConditionType_Always) {
        return ActionConditionType::kAlways;
    }
    if (condition_type == KernelActionConditionType_EventHasTarget) {
        return ActionConditionType::kEventHasTarget;
    }
    return std::nullopt;
}

ActionConditionType action_condition(const ActionGraphAction& action) {
    return std::visit(
        [](const auto& definition) { return definition.condition; },
        action);
}

const ActionGraphParameterDefinition* find_parameter_definition(
    const ActionGraphTemplate& graph,
    std::string_view name) {
    const auto found = std::find_if(
        graph.parameters.begin(),
        graph.parameters.end(),
        [name](const ActionGraphParameterDefinition& parameter) {
            return parameter.name == name;
        });
    return found == graph.parameters.end() ? nullptr : &*found;
}

const ActionGraphParameterBinding* find_parameter_binding(
    const CompiledActionGraphBinding& binding,
    std::string_view name) {
    const auto found = std::find_if(
        binding.parameters.begin(),
        binding.parameters.end(),
        [name](const ActionGraphParameterBinding& parameter) {
            return parameter.name == name;
        });
    return found == binding.parameters.end() ? nullptr : &*found;
}

std::optional<ActionGraphParameterValue> resolve_expression(
    const ActionGraphParameterExpression& expression,
    NetId self,
    const TriggerEvent& event) {
    if (const auto* value =
            std::get_if<ActionGraphParameterValue>(&expression)) {
        return *value;
    }
    if (const auto* entity_ref =
            std::get_if<EntityRefExpression>(&expression)) {
        switch (entity_ref->source) {
            case EntityRefSource::kSelf:
                return EntityIdValue{self};
            case EntityRefSource::kEventSubject:
                return EntityIdValue{event.subject};
            case EntityRefSource::kEventTarget:
                return EntityIdValue{event.target};
            case EntityRefSource::kEventInstigator:
                return EntityIdValue{event.instigator};
        }
    }
    if (std::holds_alternative<ItemRefExpression>(expression)) {
        if (!event.item_used.has_value()) return std::nullopt;
        return ItemInstanceIdValue{event.item_used->item_instance_id};
    }
    const auto* event_vec3 = std::get_if<EventVec3Expression>(&expression);
    if (event_vec3 == nullptr) {
        return std::nullopt;
    }
    switch (event_vec3->source) {
        case EventVec3Source::kPosition:
            return ActionGraphParameterValue{event.position};
        case EventVec3Source::kSubjectDirection:
            return ActionGraphParameterValue{event.subject_direction};
        case EventVec3Source::kSubjectPosition:
            return ActionGraphParameterValue{event.subject_position};
        case EventVec3Source::kDirection:
            break;
    }
    return ActionGraphParameterValue{event.direction};
}

const ActionGraphParameterValue* find_resolved_parameter(
    const std::vector<std::pair<std::string, ActionGraphParameterValue>>& parameters,
    std::string_view name) {
    const auto found = std::find_if(
        parameters.begin(),
        parameters.end(),
        [name](const auto& parameter) { return parameter.first == name; });
    return found == parameters.end() ? nullptr : &found->second;
}

bool validate_action_parameter(
    const CompiledActionGraphBinding& binding,
    std::string_view name,
    ParameterType required_type,
    std::string* error) {
    const ActionGraphParameterDefinition* parameter =
        find_parameter_definition(binding.graph, name);
    if (parameter == nullptr) {
        return fail(error, "action references undeclared parameter: " + std::string(name));
    }
    ParameterType parameter_type = value_type(parameter->default_value);
    if (const ActionGraphParameterBinding* parameter_binding =
            find_parameter_binding(binding, name)) {
        parameter_type = expression_type(parameter_binding->expression);
    }
    if (parameter_type != required_type) {
        return fail(error, "action parameter has incompatible type: " + std::string(name));
    }
    return true;
}

}  // namespace

std::optional<CompiledActionGraphBinding> compile_action_trigger_definition(
    TriggerEventType event_type,
    const KernelActionTriggerDefinition& trigger) {
    if (trigger.struct_size < sizeof(KernelActionTriggerDefinition)) {
        return std::nullopt;
    }
    const std::uint32_t action_count = trigger.action_count == 0u
        ? (trigger.action_type == KernelEntityTriggerActionType_None ? 0u : 1u)
        : trigger.action_count;
    if (action_count > KERNEL_MAX_ACTION_GRAPH_ACTIONS) {
        return std::nullopt;
    }
    CompiledActionGraphBinding binding;
    binding.event_type = event_type;
    binding.graph.id = "compiled_action_trigger";
    for (std::uint32_t index = 0; index < action_count; ++index) {
        KernelActionDefinition action{};
        if (trigger.action_count == 0u) {
            action.action_type = trigger.action_type;
            action.target_source = trigger.target_source;
            action.damage_amount = trigger.damage_amount;
            action.spawn_entity_template_id =
                trigger.spawn_entity_template_id;
            action.spawn_projectile_template_id =
                trigger.spawn_projectile_template_id;
            action.position_source = trigger.position_source;
            action.direction_source = trigger.direction_source;
            action.owner_source = trigger.owner_source;
            action.spawn_item_template_id = trigger.spawn_item_template_id;
            action.spawn_item_quantity = trigger.spawn_item_quantity;
            action.health_change_amount = trigger.health_change_amount;
            action.condition_type = trigger.condition_type;
            action.status_effect_id = trigger.status_effect_id;
            action.modifier_operation = trigger.modifier_operation;
            action.modifier_value = trigger.modifier_value;
            action.impulse_strength = trigger.impulse_strength;
            action.impulse_collision_mask = trigger.impulse_collision_mask;
            action.impulse_direction = trigger.impulse_direction;
            action.impulse_lockout_ticks = trigger.impulse_lockout_ticks;
            action.impulse_strength_mode = trigger.impulse_strength_mode;
            action.impulse_strength_vertical =
                trigger.impulse_strength_vertical;
            action.damage_stagger_authored = trigger.damage_stagger_authored;
            action.damage_stagger = trigger.damage_stagger;
        } else {
            action = trigger.actions[index];
        }
        const std::optional<ActionConditionType> condition =
            action_condition_from_kernel(action.condition_type);
        if (!condition.has_value()) {
            return std::nullopt;
        }
        const std::string suffix = "_" + std::to_string(index);
        if (action.action_type ==
            KernelEntityTriggerActionType_SpawnProjectile) {
            const std::string template_name = "projectile_template" + suffix;
            const std::string position_name = "position" + suffix;
            const std::string direction_name = "direction" + suffix;
            binding.graph.parameters.push_back({template_name, std::monostate{}});
            binding.graph.parameters.push_back({position_name, std::monostate{}});
            binding.graph.parameters.push_back({direction_name, std::monostate{}});
            ActionSpawnProjectileDefinition spawn{
                template_name,
                position_name,
                direction_name,
                *condition,
            };
            spawn.lifetime_ticks = action.spawn_lifetime_ticks;
            spawn.repeat_count_min = action.repeat_count_min;
            spawn.repeat_count_max = action.repeat_count_max;
            spawn.repeat_scatter_radius = action.repeat_scatter_radius;
            spawn.repeat_stagger_lifetime_ticks =
                action.repeat_stagger_lifetime_ticks;
            spawn.action_index = static_cast<std::uint32_t>(index);
            binding.graph.actions.push_back(std::move(spawn));
            binding.parameters.push_back({
                template_name,
                ActionGraphParameterValue{ProjectileTemplateIdValue{
                    action.spawn_projectile_template_id}},
            });
            binding.parameters.push_back({
                position_name,
                EventVec3Expression{event_vec3_source_from_kernel(
                    action.position_source)},
            });
            binding.parameters.push_back({
                direction_name,
                EventVec3Expression{event_vec3_source_from_kernel(
                    action.direction_source)},
            });
            continue;
        }
        if (action.action_type == KernelEntityTriggerActionType_SpawnEntity) {
            if (action.spawn_placement > KERNEL_SPAWN_PLACEMENT_CLEAR) {
                return std::nullopt;
            }
            const std::string template_name = "entity_template" + suffix;
            const std::string position_name = "position" + suffix;
            const std::string direction_name =
                action.direction_source == KernelEventVec3Source_Direction
                ? "direction" + suffix
                : "";
            const std::string owner_name = "owner" + suffix;
            binding.graph.parameters.push_back({template_name, std::monostate{}});
            binding.graph.parameters.push_back({position_name, std::monostate{}});
            if (!direction_name.empty()) {
                binding.graph.parameters.push_back(
                    {direction_name, std::monostate{}});
            }
            binding.graph.parameters.push_back({owner_name, std::monostate{}});
            binding.graph.actions.push_back(ActionSpawnEntityDefinition{
                template_name,
                position_name,
                direction_name,
                owner_name,
                action.spawn_item_template_id,
                action.spawn_item_quantity,
                *condition,
                action.spawn_placement,
            });
            binding.parameters.push_back({
                template_name,
                ActionGraphParameterValue{EntityTemplateIdValue{
                    action.spawn_entity_template_id}},
            });
            binding.parameters.push_back({
                position_name,
                EventVec3Expression{event_vec3_source_from_kernel(
                    action.position_source)},
            });
            if (!direction_name.empty()) {
                binding.parameters.push_back({
                    direction_name,
                    EventVec3Expression{EventVec3Source::kDirection},
                });
            }
            binding.parameters.push_back({
                owner_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.owner_source)},
            });
            continue;
        }
        if (action.action_type ==
            KernelEntityTriggerActionType_ApplySuspendMovement) {
            // On the same terms as apply_block_actions below.
            if (event_type != TriggerEventType::kStatusApplied ||
                (action.target_source != KernelEntityRefSource_Self &&
                 action.target_source != KernelEntityRefSource_EventSubject) ||
                !suspend_speed_is_authorable(action.suspend_rise_speed) ||
                !suspend_speed_is_authorable(action.suspend_drift_speed)) {
                return std::nullopt;
            }
            const std::string target_name = "target" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.actions.push_back(ActionApplySuspendMovementDefinition{
                target_name,
                action.suspend_rise_speed,
                action.suspend_drift_speed,
                *condition});
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            continue;
        }
        if (action.action_type ==
            KernelEntityTriggerActionType_ApplyBlockActions) {
            // Only a status's on_apply has an instance for the block to live
            // as long as, and only its own subject to hold.
            if (event_type != TriggerEventType::kStatusApplied ||
                (action.target_source != KernelEntityRefSource_Self &&
                 action.target_source != KernelEntityRefSource_EventSubject)) {
                return std::nullopt;
            }
            const std::string target_name = "target" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.actions.push_back(ActionApplyBlockActionsDefinition{
                target_name, *condition});
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            continue;
        }
        if (action.action_type ==
            KernelEntityTriggerActionType_RefillWeaponReserve) {
            // An item's use, for the actor using it: no other event has one.
            if (event_type != TriggerEventType::kItemUsed ||
                (action.reserve_refill_count == 0u) ==
                    (action.reserve_refill_percent == 0u) ||
                action.reserve_refill_percent > 100u ||
                action.target_source > KernelEntityRefSource_EventInstigator) {
                return std::nullopt;
            }
            const std::string target_name = "target" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.actions.push_back(ActionRefillWeaponReserveDefinition{
                target_name,
                action.reserve_refill_count,
                action.reserve_refill_percent,
                *condition,
            });
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            continue;
        }
        if (action.action_type == KernelEntityTriggerActionType_OpenUi) {
            // A building's interface opens when someone activates it, and for
            // them: no other event has an actor asking.
            if (event_type != TriggerEventType::kActivated ||
                action.ui_id == 0u ||
                action.target_source > KernelEntityRefSource_EventInstigator) {
                return std::nullopt;
            }
            const std::string target_name = "target" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.actions.push_back(ActionOpenUiDefinition{
                target_name,
                action.ui_id,
                *condition,
            });
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            continue;
        }
        if (action.action_type == KernelEntityTriggerActionType_ApplyPull) {
            if (!pull_is_authorable(
                    action.pull_mode,
                    action.pull_distance,
                    action.pull_airtime_ticks,
                    action.pull_max_speed,
                    action.pull_strength)) {
                return std::nullopt;
            }
            const std::string target_name = "target" + suffix;
            const std::string point_name = "point" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.parameters.push_back({point_name, std::monostate{}});
            binding.graph.actions.push_back(ActionApplyPullDefinition{
                target_name,
                point_name,
                action.pull_mode,
                action.pull_distance,
                action.pull_airtime_ticks,
                action.pull_max_speed,
                action.pull_strength,
                *condition,
            });
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            binding.parameters.push_back({
                point_name,
                EventVec3Expression{event_vec3_source_from_kernel(
                    action.pull_mode == KERNEL_PULL_MODE_ALONG
                        ? action.direction_source
                        : action.position_source)},
            });
            continue;
        }
        const bool applies_damage =
            action.action_type == KernelEntityTriggerActionType_ApplyDamage;
        const bool applies_health_change =
            action.action_type ==
            KernelEntityTriggerActionType_ApplyHealthChange;
        const bool applies_impulse =
            action.action_type == KernelEntityTriggerActionType_ApplyImpulse;
        const bool applies_status =
            action.action_type == KernelEntityTriggerActionType_ApplyStatus;
        const bool removes_status =
            action.action_type == KernelEntityTriggerActionType_RemoveStatus;
        const bool applies_speed_modifier =
            action.action_type ==
            KernelEntityTriggerActionType_ApplySpeedModifier;
        if (!applies_damage && !applies_health_change && !applies_impulse &&
            !applies_status && !removes_status && !applies_speed_modifier) {
            return std::nullopt;
        }
        if (applies_status || removes_status) {
            const std::string target_name = "target" + suffix;
            const std::string status_name = "status" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.parameters.push_back({
                status_name,
                StatusEffectIdValue{action.status_effect_id},
            });
            const bool carries_direction =
                applies_status && action.status_direction_authored != 0u;
            if (carries_direction &&
                action.direction_source > KernelEventVec3Source_SubjectPosition) {
                return std::nullopt;
            }
            const std::string direction_name = "direction" + suffix;
            binding.graph.actions.push_back(
                applies_status
                    ? ActionGraphAction{ActionApplyStatusDefinition{
                          target_name, status_name, *condition,
                          carries_direction ? direction_name : std::string{}}}
                    : ActionGraphAction{ActionRemoveStatusDefinition{
                          target_name, status_name, *condition}});
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            if (carries_direction) {
                binding.graph.parameters.push_back({direction_name, std::monostate{}});
                if (action.direction_source == KernelEventVec3Source_Literal) {
                    binding.parameters.push_back({
                        direction_name,
                        ActionGraphParameterValue{glm::vec3{
                            action.impulse_direction.x,
                            action.impulse_direction.y,
                            action.impulse_direction.z}},
                    });
                } else {
                    binding.parameters.push_back({
                        direction_name,
                        EventVec3Expression{event_vec3_source_from_kernel(
                            action.direction_source)},
                    });
                }
            }
            continue;
        }
        if (applies_speed_modifier) {
            const std::string target_name = "target" + suffix;
            const std::string operation_name = "operation" + suffix;
            const std::string value_name = "value" + suffix;
            binding.graph.parameters.push_back({target_name, std::monostate{}});
            binding.graph.parameters.push_back({operation_name, static_cast<float>(
                action.modifier_operation)});
            binding.graph.parameters.push_back({value_name, action.modifier_value});
            binding.graph.actions.push_back(ActionApplySpeedModifierDefinition{
                target_name, operation_name, value_name, *condition});
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            continue;
        }
        const std::string target_name = "target" + suffix;
        const std::string amount_name = "amount" + suffix;
        binding.graph.parameters.push_back({target_name, std::monostate{}});
        if (applies_impulse) {
            const std::string direction_name = "direction" + suffix;
            binding.graph.parameters.push_back({
                "strength" + suffix,
                static_cast<float>(action.impulse_strength),
            });
            binding.graph.parameters.push_back({
                direction_name,
                std::monostate{},
            });
            binding.graph.actions.push_back(ActionApplyImpulseDefinition{
                target_name,
                "strength" + suffix,
                direction_name,
                action.impulse_collision_mask,
                action.impulse_lockout_ticks,
                action.impulse_strength_mode,
                action.impulse_strength_vertical,
                action.direction_source == KernelEventVec3Source_Literal
                    ? std::optional<glm::vec3>{glm::vec3{
                          action.impulse_direction.x,
                          action.impulse_direction.y,
                          action.impulse_direction.z}}
                    : std::nullopt,
                *condition,
            });
            binding.parameters.push_back({
                target_name,
                EntityRefExpression{static_cast<EntityRefSource>(
                    action.target_source)},
            });
            if (action.direction_source != KernelEventVec3Source_Literal) {
                binding.parameters.push_back({
                    direction_name,
                    EventVec3Expression{event_vec3_source_from_kernel(
                        action.direction_source)},
                });
            }
            continue;
        }
        binding.graph.parameters.push_back({
            amount_name,
            applies_damage
                ? static_cast<float>(action.damage_amount)
                : static_cast<float>(action.health_change_amount),
        });
        if (applies_damage) {
            binding.graph.actions.push_back(ActionApplyDamageDefinition{
                target_name,
                amount_name,
                *condition,
                action.damage_stagger_authored != 0u
                    ? std::optional<float>{action.damage_stagger}
                    : std::nullopt,
            });
        } else {
            binding.graph.actions.push_back(ActionApplyHealthChangeDefinition{
                target_name,
                amount_name,
                *condition,
            });
        }
        binding.parameters.push_back({
            target_name,
            EntityRefExpression{static_cast<EntityRefSource>(
                action.target_source)},
        });
    }
    if (!validate_action_graph_binding(binding, nullptr)) {
        return std::nullopt;
    }
    return binding;
}

CompiledActionGraphBinding compile_spawn_projectile_binding(
    TriggerEventType event_type,
    std::uint32_t projectile_template_id) {
    return CompiledActionGraphBinding{
        event_type,
        ActionGraphTemplate{
            event_type == TriggerEventType::kExpired
                ? "action_spawn_projectile_at_expired"
                : "action_spawn_projectile_at_impact",
            {
                {"template", std::monostate{}},
                {"position", std::monostate{}},
                {"direction", std::monostate{}},
            },
            {ActionSpawnProjectileDefinition{
                "template",
                "position",
                "direction",
            }},
        },
        {
            {"template", ActionGraphParameterValue{
                 ProjectileTemplateIdValue{projectile_template_id}}},
            {"position", EventVec3Expression{EventVec3Source::kPosition}},
            {"direction", EventVec3Expression{EventVec3Source::kDirection}},
        },
    };
}

CompiledActionGraphBinding compile_apply_damage_binding(
    TriggerEventType event_type,
    EntityRefSource target_source,
    std::uint16_t amount) {
    const std::string graph_id = [&]() {
        switch (event_type) {
            case TriggerEventType::kCollision:
                return "action_apply_damage_at_collision";
            case TriggerEventType::kHealthDepleted:
                return "action_apply_damage_at_health_depleted";
            case TriggerEventType::kDestroyEntity:
                return "action_apply_damage_at_destroy_entity";
            default:
                return "action_apply_damage_at_activated";
        }
    }();
    return CompiledActionGraphBinding{
        event_type,
        ActionGraphTemplate{
            graph_id,
            {
                {"target", std::monostate{}},
                {"amount", static_cast<float>(amount)},
            },
            {ActionApplyDamageDefinition{"target", "amount"}},
        },
        {
            {"target", EntityRefExpression{target_source}},
        },
    };
}

// Keep this binding generic for all entity-template-backed spawns, including
// props, obstacles, pickups, and deployables. Future rotation and placement
// validation should live in spawn parameters and a placement policy/system,
// rather than introducing specialized compilers such as
// compile_spawn_prop_binding().
CompiledActionGraphBinding compile_spawn_entity_binding(
    TriggerEventType event_type,
    std::uint32_t entity_template_id,
    EventVec3Source position_source,
    EntityRefSource owner_source) {
    const std::string graph_id = [&]() {
        switch (event_type) {
            case TriggerEventType::kCollision:
                return "action_spawn_entity_at_collision";
            case TriggerEventType::kHealthDepleted:
                return "action_spawn_entity_at_health_depleted";
            case TriggerEventType::kDestroyEntity:
                return "action_spawn_entity_at_destroy_entity";
            default:
                return "action_spawn_entity_at_activated";
        }
    }();
    return CompiledActionGraphBinding{
        event_type,
        ActionGraphTemplate{
            graph_id,
            {
                {"template", std::monostate{}},
                {"position", std::monostate{}},
                {"owner", std::monostate{}},
            },
            {ActionSpawnEntityDefinition{
                "template",
                "position",
                "",
                "owner",
            }},
        },
        {
            {"template", ActionGraphParameterValue{
                 EntityTemplateIdValue{entity_template_id}}},
            {"position", EventVec3Expression{position_source}},
            {"owner", EntityRefExpression{owner_source}},
        },
    };
}

bool validate_action_graph_binding(
    const CompiledActionGraphBinding& binding,
    std::string* error) {
    if (binding.graph.id.empty()) {
        return fail(error, "action graph id must not be empty");
    }
    for (const ActionGraphParameterBinding& parameter : binding.parameters) {
        const ActionGraphParameterDefinition* definition =
            find_parameter_definition(binding.graph, parameter.name);
        if (definition == nullptr) {
            return fail(error, "binding passes undeclared parameter: " + parameter.name);
        }
        const ParameterType default_type = value_type(definition->default_value);
        const ParameterType binding_type = expression_type(parameter.expression);
        if (default_type != ParameterType::kUnknown &&
            default_type != binding_type) {
            return fail(error, "binding parameter has incompatible type: " + parameter.name);
        }
        if (!expression_available_for_event(
                parameter.expression, binding.event_type)) {
            return fail(
                error,
                "binding expression is unavailable for trigger event: " +
                    parameter.name);
        }
    }
    for (const ActionGraphParameterDefinition& parameter :
         binding.graph.parameters) {
        if (std::holds_alternative<std::monostate>(parameter.default_value) &&
            find_parameter_binding(binding, parameter.name) == nullptr) {
            return fail(error, "required action graph parameter is missing: " + parameter.name);
        }
    }
    for (const ActionGraphAction& action : binding.graph.actions) {
        if (const auto* spawn =
                std::get_if<ActionSpawnProjectileDefinition>(&action)) {
            if (!validate_action_parameter(
                binding,
                spawn->projectile_template_parameter,
                ParameterType::kProjectileTemplateId,
                error) ||
            !validate_action_parameter(
                binding,
                spawn->position_parameter,
                ParameterType::kVec3,
                error) ||
            !validate_action_parameter(
                binding,
                spawn->direction_parameter,
                ParameterType::kVec3,
                error)) {
                return false;
            }
            continue;
        }
        if (const auto* spawn =
                std::get_if<ActionSpawnEntityDefinition>(&action)) {
            if (!validate_action_parameter(
                    binding,
                    spawn->entity_template_parameter,
                    ParameterType::kEntityTemplateId,
                    error) ||
                !validate_action_parameter(
                    binding,
                    spawn->position_parameter,
                    ParameterType::kVec3,
                    error) ||
                (!spawn->direction_parameter.empty() &&
                 !validate_action_parameter(
                     binding,
                     spawn->direction_parameter,
                     ParameterType::kVec3,
                     error)) ||
                !validate_action_parameter(
                    binding,
                    spawn->owner_parameter,
                    ParameterType::kEntityId,
                    error)) {
                return false;
            }
            continue;
        }
        if (const auto* suspend =
                std::get_if<ActionApplySuspendMovementDefinition>(&action)) {
            if (!suspend_speed_is_authorable(suspend->rise_speed) ||
                !suspend_speed_is_authorable(suspend->drift_speed)) {
                return fail(
                    error, "apply_suspend_movement speeds must be finite, 0 to 20 m/s");
            }
            if (!validate_action_parameter(
                    binding, suspend->target_parameter, ParameterType::kEntityId,
                    error)) {
                return false;
            }
            continue;
        }
        if (const auto* block =
                std::get_if<ActionApplyBlockActionsDefinition>(&action)) {
            if (!validate_action_parameter(
                    binding, block->target_parameter, ParameterType::kEntityId,
                    error)) {
                return false;
            }
            continue;
        }
        if (const auto* refill =
                std::get_if<ActionRefillWeaponReserveDefinition>(&action)) {
            if ((refill->count == 0u) == (refill->percent == 0u) ||
                refill->percent > 100u) {
                return fail(
                    error,
                    "refill_weapon_reserve needs exactly one of count or "
                    "percent (1-100)");
            }
            if (!validate_action_parameter(
                    binding, refill->target_parameter, ParameterType::kEntityId,
                    error)) {
                return false;
            }
            continue;
        }
        if (const auto* open_ui = std::get_if<ActionOpenUiDefinition>(&action)) {
            if (open_ui->ui_id == 0u) {
                return fail(error, "open_ui requires a non-zero ui_id");
            }
            if (!validate_action_parameter(
                    binding, open_ui->target_parameter, ParameterType::kEntityId,
                    error)) {
                return false;
            }
            continue;
        }
        if (const auto* pull = std::get_if<ActionApplyPullDefinition>(&action)) {
            if (!pull_is_authorable(
                    pull->mode, pull->distance, pull->airtime_ticks,
                    pull->max_speed, pull->strength)) {
                return fail(error, "apply_pull distance, airtime, max_speed or strength out of range");
            }
            if (!validate_action_parameter(
                    binding, pull->target_parameter, ParameterType::kEntityId, error) ||
                !validate_action_parameter(
                    binding, pull->point_parameter, ParameterType::kVec3, error)) {
                return false;
            }
            continue;
        }
        const auto* damage = std::get_if<ActionApplyDamageDefinition>(&action);
        const auto* health_change =
            std::get_if<ActionApplyHealthChangeDefinition>(&action);
        const auto* impulse =
            std::get_if<ActionApplyImpulseDefinition>(&action);
        const auto* apply_status =
            std::get_if<ActionApplyStatusDefinition>(&action);
        const auto* remove_status =
            std::get_if<ActionRemoveStatusDefinition>(&action);
        const auto* speed_modifier =
            std::get_if<ActionApplySpeedModifierDefinition>(&action);
        if (apply_status != nullptr || remove_status != nullptr) {
            const std::string& target_parameter = apply_status != nullptr
                ? apply_status->target_parameter
                : remove_status->target_parameter;
            const std::string& status_parameter = apply_status != nullptr
                ? apply_status->status_parameter
                : remove_status->status_parameter;
            if (!validate_action_parameter(
                    binding, target_parameter, ParameterType::kEntityId, error) ||
                !validate_action_parameter(
                    binding, status_parameter, ParameterType::kStatusEffectId, error)) {
                return false;
            }
            if (apply_status != nullptr &&
                !apply_status->direction_parameter.empty() &&
                !validate_action_parameter(
                    binding, apply_status->direction_parameter,
                    ParameterType::kVec3, error)) {
                return false;
            }
            continue;
        }
        if (speed_modifier != nullptr) {
            if (!validate_action_parameter(
                    binding, speed_modifier->target_parameter,
                    ParameterType::kEntityId, error) ||
                !validate_action_parameter(
                    binding, speed_modifier->operation_parameter,
                    ParameterType::kNumber, error) ||
                !validate_action_parameter(
                    binding, speed_modifier->value_parameter,
                    ParameterType::kNumber, error)) {
                return false;
            }
            continue;
        }
        const std::string* target_parameter = damage != nullptr
            ? &damage->target_parameter
            : health_change != nullptr ? &health_change->target_parameter
            : impulse != nullptr ? &impulse->target_parameter : nullptr;
        const std::string* amount_parameter = damage != nullptr
            ? &damage->amount_parameter
            : health_change != nullptr ? &health_change->amount_parameter
            : impulse != nullptr ? &impulse->strength_parameter : nullptr;
        if (target_parameter == nullptr || amount_parameter == nullptr ||
            !validate_action_parameter(
                binding,
                *target_parameter,
                ParameterType::kEntityId,
                error) ||
            !validate_action_parameter(
                binding,
                *amount_parameter,
                ParameterType::kNumber,
                error)) {
            return false;
        }
        if (impulse != nullptr &&
            !validate_action_parameter(
                binding,
                impulse->direction_parameter,
                ParameterType::kVec3,
                error)) {
            return false;
        }
    }
    return true;
}

bool evaluate_action_graph(
    const CompiledActionGraphBinding& binding,
    NetId self,
    const TriggerEvent& event,
    const ActionExecutionProvenance& provenance,
    std::vector<ActionGraphCommand>* commands,
    std::string* error) {
    if (commands == nullptr) {
        return fail(error, "action graph command output must not be null");
    }
    if (binding.event_type != event.type) {
        return fail(error, "trigger event does not match compiled binding");
    }
    if (!validate_action_graph_binding(binding, error)) {
        return false;
    }

    std::vector<std::pair<std::string, ActionGraphParameterValue>> parameters;
    parameters.reserve(binding.graph.parameters.size());
    for (const ActionGraphParameterDefinition& definition :
         binding.graph.parameters) {
        ActionGraphParameterValue value = definition.default_value;
        if (const ActionGraphParameterBinding* parameter_binding =
                find_parameter_binding(binding, definition.name)) {
            const std::optional<ActionGraphParameterValue> resolved =
                resolve_expression(parameter_binding->expression, self, event);
            if (!resolved.has_value()) {
                return fail(error, "could not resolve binding parameter: " + definition.name);
            }
            value = *resolved;
        }
        parameters.emplace_back(definition.name, std::move(value));
    }

    if (provenance.authority_source !=
        ActionAuthoritySource::kAuthoritativeSimulation) {
        return true;
    }
    for (const ActionGraphAction& action : binding.graph.actions) {
        if (action_condition(action) == ActionConditionType::kEventHasTarget &&
            event.target == 0u) {
            continue;
        }
        if (const auto* spawn =
                std::get_if<ActionSpawnProjectileDefinition>(&action)) {
            const ActionGraphParameterValue* template_value =
                find_resolved_parameter(
                    parameters, spawn->projectile_template_parameter);
            const ActionGraphParameterValue* position_value =
                find_resolved_parameter(parameters, spawn->position_parameter);
            const ActionGraphParameterValue* direction_value =
                find_resolved_parameter(parameters, spawn->direction_parameter);
            if (template_value == nullptr || position_value == nullptr ||
                direction_value == nullptr ||
                !std::holds_alternative<ProjectileTemplateIdValue>(
                    *template_value) ||
                !std::holds_alternative<glm::vec3>(*position_value) ||
                !std::holds_alternative<glm::vec3>(*direction_value)) {
                return fail(error, "spawn_projectile action input type mismatch");
            }
            const std::uint32_t projectile_template_id =
                std::get<ProjectileTemplateIdValue>(*template_value).value;
            const glm::vec3 position = std::get<glm::vec3>(*position_value);
            const glm::vec3 direction = std::get<glm::vec3>(*direction_value);
            if (spawn->repeat_count_max == 0u) {
                ActionSpawnProjectileCommand command{
                    projectile_template_id, position, direction, provenance};
                command.lifetime_ticks = spawn->lifetime_ticks;
                commands->push_back(command);
                continue;
            }
            append_repeated_spawns(
                *spawn,
                projectile_template_id,
                position,
                direction,
                provenance,
                commands);
            continue;
        }

        if (const auto* spawn =
                std::get_if<ActionSpawnEntityDefinition>(&action)) {
            const ActionGraphParameterValue* template_value =
                find_resolved_parameter(
                    parameters, spawn->entity_template_parameter);
            const ActionGraphParameterValue* position_value =
                find_resolved_parameter(parameters, spawn->position_parameter);
            const ActionGraphParameterValue* direction_value =
                spawn->direction_parameter.empty()
                ? nullptr
                : find_resolved_parameter(
                      parameters, spawn->direction_parameter);
            const ActionGraphParameterValue* owner_value =
                find_resolved_parameter(parameters, spawn->owner_parameter);
            if (template_value == nullptr || position_value == nullptr ||
                owner_value == nullptr ||
                (!spawn->direction_parameter.empty() &&
                 (direction_value == nullptr ||
                  !std::holds_alternative<glm::vec3>(*direction_value))) ||
                !std::holds_alternative<EntityTemplateIdValue>(
                    *template_value) ||
                !std::holds_alternative<glm::vec3>(*position_value) ||
                !std::holds_alternative<EntityIdValue>(*owner_value)) {
                return fail(error, "spawn_entity action input type mismatch");
            }
            const std::uint32_t entity_template_id =
                std::get<EntityTemplateIdValue>(*template_value).value;
            const glm::vec3 position = std::get<glm::vec3>(*position_value);
            const glm::vec3 direction =
                direction_value == nullptr
                ? glm::vec3{0.0f}
                : std::get<glm::vec3>(*direction_value);
            const NetId owner = std::get<EntityIdValue>(*owner_value).value;
            if (entity_template_id == 0u || owner == 0u ||
                !std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z) ||
                !std::isfinite(direction.x) ||
                !std::isfinite(direction.y) ||
                !std::isfinite(direction.z)) {
                return fail(
                    error,
                    "spawn_entity requires a template, owner, finite position, and finite direction");
            }
            commands->push_back(ActionSpawnEntityCommand{
                entity_template_id,
                position,
                direction,
                owner,
                spawn->item_template_id,
                spawn->quantity,
                provenance,
                spawn->placement,
            });
            continue;
        }

        if (const auto* suspend =
                std::get_if<ActionApplySuspendMovementDefinition>(&action)) {
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, suspend->target_parameter);
            if (target_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value)) {
                return fail(error, "apply_suspend_movement action input type mismatch");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "apply_suspend_movement target must not be null");
            }
            // The status's direction, which its lifecycle event carries.
            commands->push_back(ActionApplySuspendMovementCommand{
                action_source(self, event),
                target,
                provenance.status_instance_id,
                suspend->rise_speed,
                suspend_drift_velocity(event.direction, suspend->drift_speed),
                provenance,
            });
            continue;
        }
        if (const auto* block =
                std::get_if<ActionApplyBlockActionsDefinition>(&action)) {
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, block->target_parameter);
            if (target_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value)) {
                return fail(error, "apply_block_actions action input type mismatch");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "apply_block_actions target must not be null");
            }
            commands->push_back(ActionApplyBlockActionsCommand{
                action_source(self, event),
                target,
                provenance.status_instance_id,
                provenance,
            });
            continue;
        }

        if (const auto* refill =
                std::get_if<ActionRefillWeaponReserveDefinition>(&action)) {
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, refill->target_parameter);
            if (target_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value)) {
                return fail(error, "refill_weapon_reserve action input type mismatch");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "refill_weapon_reserve target must not be null");
            }
            commands->push_back(ActionRefillWeaponReserveCommand{
                self,
                target,
                refill->count,
                refill->percent,
                provenance,
            });
            continue;
        }

        if (const auto* open_ui = std::get_if<ActionOpenUiDefinition>(&action)) {
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, open_ui->target_parameter);
            if (target_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value)) {
                return fail(error, "open_ui action input type mismatch");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "open_ui target must not be null");
            }
            commands->push_back(ActionOpenUiCommand{
                self,
                target,
                open_ui->ui_id,
                provenance,
            });
            continue;
        }

        if (const auto* pull = std::get_if<ActionApplyPullDefinition>(&action)) {
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, pull->target_parameter);
            const ActionGraphParameterValue* point_value =
                find_resolved_parameter(parameters, pull->point_parameter);
            if (target_value == nullptr || point_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value) ||
                !std::holds_alternative<glm::vec3>(*point_value)) {
                return fail(error, "apply_pull action input type mismatch");
            }
            const glm::vec3 point = std::get<glm::vec3>(*point_value);
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                return fail(error, "apply_pull requires a finite point or direction");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "apply_pull target must not be null");
            }
            commands->push_back(ActionApplyPullCommand{
                self,
                target,
                pull->mode,
                point,
                pull->distance,
                pull->airtime_ticks,
                pull->max_speed,
                pull->strength,
                provenance,
            });
            continue;
        }

        const auto* damage = std::get_if<ActionApplyDamageDefinition>(&action);
        const auto* health_change =
            std::get_if<ActionApplyHealthChangeDefinition>(&action);
        const auto* impulse =
            std::get_if<ActionApplyImpulseDefinition>(&action);
        const auto* apply_status =
            std::get_if<ActionApplyStatusDefinition>(&action);
        const auto* remove_status =
            std::get_if<ActionRemoveStatusDefinition>(&action);
        const auto* speed_modifier =
            std::get_if<ActionApplySpeedModifierDefinition>(&action);
        if (apply_status != nullptr || remove_status != nullptr) {
            const std::string& target_parameter = apply_status != nullptr
                ? apply_status->target_parameter
                : remove_status->target_parameter;
            const std::string& status_parameter = apply_status != nullptr
                ? apply_status->status_parameter
                : remove_status->status_parameter;
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, target_parameter);
            const ActionGraphParameterValue* status_value =
                find_resolved_parameter(parameters, status_parameter);
            if (target_value == nullptr || status_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value) ||
                !std::holds_alternative<StatusEffectIdValue>(*status_value)) {
                return fail(error, "status action input type mismatch");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            const std::uint32_t status_id =
                std::get<StatusEffectIdValue>(*status_value).value;
            if (target == 0u || status_id == 0u) {
                return fail(error, "status action requires target and status");
            }
            const NetId source = action_source(self, event);
            if (apply_status != nullptr) {
                glm::vec3 direction{0.0f};
                if (!apply_status->direction_parameter.empty()) {
                    const ActionGraphParameterValue* direction_value =
                        find_resolved_parameter(
                            parameters, apply_status->direction_parameter);
                    if (direction_value == nullptr ||
                        !std::holds_alternative<glm::vec3>(*direction_value)) {
                        return fail(error, "apply_status direction must be a vec3");
                    }
                    direction = std::get<glm::vec3>(*direction_value);
                    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
                        !std::isfinite(direction.z)) {
                        return fail(error, "apply_status direction must be finite");
                    }
                }
                commands->push_back(ActionApplyStatusCommand{
                    source, target, status_id, provenance, direction});
            } else {
                commands->push_back(ActionRemoveStatusCommand{
                    source, target, status_id, provenance});
            }
            continue;
        }
        if (speed_modifier != nullptr) {
            const ActionGraphParameterValue* target_value =
                find_resolved_parameter(parameters, speed_modifier->target_parameter);
            const ActionGraphParameterValue* operation_value =
                find_resolved_parameter(parameters, speed_modifier->operation_parameter);
            const ActionGraphParameterValue* value_value =
                find_resolved_parameter(parameters, speed_modifier->value_parameter);
            if (target_value == nullptr || operation_value == nullptr ||
                value_value == nullptr ||
                !std::holds_alternative<EntityIdValue>(*target_value) ||
                !std::holds_alternative<float>(*operation_value) ||
                !std::holds_alternative<float>(*value_value)) {
                return fail(error, "apply_speed_modifier action input type mismatch");
            }
            const float operation = std::get<float>(*operation_value);
            const float value = std::get<float>(*value_value);
            if (!std::isfinite(operation) || !std::isfinite(value) ||
                std::floor(operation) != operation || operation < 0.0f ||
                operation > 1.0f || !std::isfinite(value)) {
                return fail(error, "apply_speed_modifier requires valid operation and value");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "apply_speed_modifier target must not be null");
            }
            ActionExecutionProvenance modifier_provenance = provenance;
            const NetId source = action_source(self, event);
            modifier_provenance.instigator = source;
            commands->push_back(ActionApplySpeedModifierCommand{
                source,
                target,
                provenance.status_instance_id,
                static_cast<std::uint8_t>(operation),
                value,
                modifier_provenance,
            });
            continue;
        }
        if (damage == nullptr && health_change == nullptr && impulse == nullptr) {
            return fail(error, "unsupported action graph action");
        }
        const std::string& target_parameter = damage != nullptr
            ? damage->target_parameter
            : health_change != nullptr ? health_change->target_parameter
            : impulse->target_parameter;
        const std::string& amount_parameter = damage != nullptr
            ? damage->amount_parameter
            : health_change != nullptr ? health_change->amount_parameter
            : impulse->strength_parameter;
        const ActionGraphParameterValue* target_value =
            find_resolved_parameter(parameters, target_parameter);
        const ActionGraphParameterValue* amount_value =
            find_resolved_parameter(parameters, amount_parameter);
        if (target_value == nullptr || amount_value == nullptr ||
            !std::holds_alternative<EntityIdValue>(*target_value) ||
            !std::holds_alternative<float>(*amount_value)) {
            return fail(
                error,
                damage != nullptr
                    ? "apply_damage action input type mismatch"
                    : health_change != nullptr
                    ? "apply_health_change action input type mismatch"
                    : "apply_impulse action input type mismatch");
        }
        const float amount = std::get<float>(*amount_value);
        if (!std::isfinite(amount)) {
            return fail(error, "health action amount must be a finite integer");
        }
        if (impulse != nullptr) {
            const ActionGraphParameterValue* direction_value =
                find_resolved_parameter(parameters, impulse->direction_parameter);
            if (direction_value == nullptr ||
                !std::holds_alternative<glm::vec3>(*direction_value)) {
                return fail(error, "apply_impulse action input type mismatch");
            }
            const glm::vec3 direction = std::get<glm::vec3>(*direction_value);
            const float direction_length = glm::length(direction);
            if (!impulse_strength_is_authorable(
                    impulse->strength_mode, amount, impulse->vertical_strength) ||
                direction_length <= 0.0f ||
                !std::isfinite(direction.x) || !std::isfinite(direction.y) ||
                !std::isfinite(direction.z)) {
                return fail(error, "apply_impulse requires positive strength and a finite non-zero direction");
            }
            const NetId target = std::get<EntityIdValue>(*target_value).value;
            if (target == 0u) {
                return fail(error, "apply_impulse target must not be null");
            }
            const NetId source = event.type == TriggerEventType::kItemUsed && self == 0u
                ? event.instigator
                : self;
            commands->push_back(ActionApplyImpulseCommand{
                source,
                target,
                amount,
                direction / direction_length,
                impulse->collision_mask,
                impulse->lockout_ticks,
                impulse->strength_mode,
                impulse->vertical_strength,
                provenance,
            });
            continue;
        }
        if (std::floor(amount) != amount) {
            return fail(error, "health action amount must be a finite integer");
        }
        if (damage != nullptr &&
            (amount <= 0.0f ||
             amount > static_cast<float>(
                 std::numeric_limits<std::uint16_t>::max()))) {
            return fail(error, "apply_damage amount must be a positive uint16");
        }
        if (health_change != nullptr &&
            (amount == 0.0f ||
             amount < -static_cast<float>(
                 std::numeric_limits<std::uint16_t>::max()) ||
             amount > static_cast<float>(
                 std::numeric_limits<std::uint16_t>::max()))) {
            return fail(
                error,
                "apply_health_change amount must be a non-zero signed uint16 range");
        }
        const NetId target = std::get<EntityIdValue>(*target_value).value;
        if (target == 0u) {
            return fail(error, "health action target must not be null");
        }
        const NetId source = action_source(self, event);
        if (damage != nullptr) {
            commands->push_back(ActionApplyDamageCommand{
                source,
                target,
                static_cast<std::uint16_t>(amount),
                provenance,
                damage->stagger.value_or(kStaggerDerivedFromDamage),
            });
        } else {
            commands->push_back(ActionApplyHealthChangeCommand{
                source,
                target,
                static_cast<std::int32_t>(amount),
                provenance,
            });
        }
    }
    return true;
}

bool dispatch_action_graph_triggers(
    std::vector<ActionGraphQueuedTrigger>* queued_triggers,
    std::vector<ActionGraphCommandBatch>* command_batches,
    std::string* error) {
    if (queued_triggers == nullptr || command_batches == nullptr) {
        return fail(error, "action graph dispatcher input must not be null");
    }
    std::sort(
        queued_triggers->begin(),
        queued_triggers->end(),
        [](const ActionGraphQueuedTrigger& lhs,
           const ActionGraphQueuedTrigger& rhs) {
            if (lhs.provenance.server_tick != rhs.provenance.server_tick) {
                return lhs.provenance.server_tick < rhs.provenance.server_tick;
            }
            if (lhs.event.subject != rhs.event.subject) {
                return lhs.event.subject < rhs.event.subject;
            }
            if (lhs.sequence != rhs.sequence) {
                return lhs.sequence < rhs.sequence;
            }
            if (lhs.event.type != rhs.event.type) {
                return lhs.event.type < rhs.event.type;
            }
            if (lhs.event.target != rhs.event.target) {
                return lhs.event.target < rhs.event.target;
            }
            return lhs.provenance.request_id < rhs.provenance.request_id;
        });

    std::vector<ActionGraphCommandBatch> dispatched;
    dispatched.reserve(queued_triggers->size());
    for (const ActionGraphQueuedTrigger& queued : *queued_triggers) {
        ActionGraphCommandBatch batch{
            queued.event,
            queued.provenance,
            queued.sequence,
            {},
        };
        if (!evaluate_action_graph(
                queued.binding,
                queued.self,
                queued.event,
                queued.provenance,
                &batch.commands,
                error)) {
            return false;
        }
        dispatched.push_back(std::move(batch));
    }
    *command_batches = std::move(dispatched);
    queued_triggers->clear();
    return true;
}

}  // namespace network_example
