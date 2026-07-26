#include "simulation/public/action_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace network_example {
namespace {

enum class ParameterType : std::uint8_t {
    kUnknown,
    kEntityId,
    kProjectileTemplateId,
    kEntityTemplateId,
    kVec3,
    kNumber,
};

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
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
    return ParameterType::kVec3;
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
    const auto* event_vec3 = std::get_if<EventVec3Expression>(&expression);
    if (event_vec3 == nullptr) {
        return std::nullopt;
    }
    return event_vec3->source == EventVec3Source::kPosition
        ? ActionGraphParameterValue{event.position}
        : ActionGraphParameterValue{event.direction};
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
                !validate_action_parameter(
                    binding,
                    spawn->owner_parameter,
                    ParameterType::kEntityId,
                    error)) {
                return false;
            }
            continue;
        }
        const auto* damage = std::get_if<ActionApplyDamageDefinition>(&action);
        if (damage == nullptr ||
            !validate_action_parameter(
                binding,
                damage->target_parameter,
                ParameterType::kEntityId,
                error) ||
            !validate_action_parameter(
                binding,
                damage->amount_parameter,
                ParameterType::kNumber,
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
            commands->push_back(ActionSpawnProjectileCommand{
                std::get<ProjectileTemplateIdValue>(*template_value).value,
                std::get<glm::vec3>(*position_value),
                std::get<glm::vec3>(*direction_value),
                provenance,
            });
            continue;
        }

        if (const auto* spawn =
                std::get_if<ActionSpawnEntityDefinition>(&action)) {
            const ActionGraphParameterValue* template_value =
                find_resolved_parameter(
                    parameters, spawn->entity_template_parameter);
            const ActionGraphParameterValue* position_value =
                find_resolved_parameter(parameters, spawn->position_parameter);
            const ActionGraphParameterValue* owner_value =
                find_resolved_parameter(parameters, spawn->owner_parameter);
            if (template_value == nullptr || position_value == nullptr ||
                owner_value == nullptr ||
                !std::holds_alternative<EntityTemplateIdValue>(
                    *template_value) ||
                !std::holds_alternative<glm::vec3>(*position_value) ||
                !std::holds_alternative<EntityIdValue>(*owner_value)) {
                return fail(error, "spawn_entity action input type mismatch");
            }
            const std::uint32_t entity_template_id =
                std::get<EntityTemplateIdValue>(*template_value).value;
            const glm::vec3 position = std::get<glm::vec3>(*position_value);
            const NetId owner = std::get<EntityIdValue>(*owner_value).value;
            if (entity_template_id == 0u || owner == 0u ||
                !std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z)) {
                return fail(
                    error,
                    "spawn_entity requires a template, owner, and finite position");
            }
            commands->push_back(ActionSpawnEntityCommand{
                entity_template_id,
                position,
                owner,
                provenance,
            });
            continue;
        }

        const auto* damage = std::get_if<ActionApplyDamageDefinition>(&action);
        if (damage == nullptr) {
            return fail(error, "unsupported action graph action");
        }
        const ActionGraphParameterValue* target_value =
            find_resolved_parameter(parameters, damage->target_parameter);
        const ActionGraphParameterValue* amount_value =
            find_resolved_parameter(parameters, damage->amount_parameter);
        if (target_value == nullptr || amount_value == nullptr ||
            !std::holds_alternative<EntityIdValue>(*target_value) ||
            !std::holds_alternative<float>(*amount_value)) {
            return fail(error, "apply_damage action input type mismatch");
        }
        const float amount = std::get<float>(*amount_value);
        if (!std::isfinite(amount) || amount <= 0.0f ||
            amount > static_cast<float>(std::numeric_limits<std::uint16_t>::max()) ||
            std::floor(amount) != amount) {
            return fail(error, "apply_damage amount must be a positive uint16");
        }
        const NetId target = std::get<EntityIdValue>(*target_value).value;
        if (target == 0u) {
            return fail(error, "apply_damage target must not be null");
        }
        commands->push_back(ActionApplyDamageCommand{
            self,
            target,
            static_cast<std::uint16_t>(amount),
            provenance,
        });
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
