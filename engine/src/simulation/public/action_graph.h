#ifndef SIMULATION_PUBLIC_ACTION_GRAPH_H_
#define SIMULATION_PUBLIC_ACTION_GRAPH_H_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "world/public/components.h"

namespace network_example {

struct ActionSpawnProjectileCommand {
    std::uint32_t projectile_template_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
    ActionExecutionProvenance provenance;
};

struct ActionApplyDamageCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint16_t amount = 0;
    ActionExecutionProvenance provenance;
};

struct ActionApplyHealthChangeCommand {
    NetId source = 0;
    NetId target = 0;
    std::int32_t amount = 0;
    ActionExecutionProvenance provenance;
};

struct ActionApplyImpulseCommand {
    NetId source = 0;
    NetId target = 0;
    float strength = 0.0f;
    glm::vec3 direction{0.0f};
    std::uint32_t collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    ActionExecutionProvenance provenance;
};

struct ActionApplyStatusCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t status_effect_id = 0;
    ActionExecutionProvenance provenance;
};

struct ActionRemoveStatusCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t status_effect_id = 0;
    ActionExecutionProvenance provenance;
};

struct ActionApplySpeedModifierCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t status_instance_id = 0;
    std::uint8_t operation = KernelStatModifierOperation_Additive;
    float value = 0.0f;
    ActionExecutionProvenance provenance;
};

struct ActionSpawnEntityCommand {
    std::uint32_t entity_template_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
    NetId owner = 0;
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    ActionExecutionProvenance provenance;
};

using ActionGraphCommand = std::variant<
    ActionSpawnProjectileCommand,
    ActionApplyDamageCommand,
    ActionApplyHealthChangeCommand,
    ActionApplyImpulseCommand,
    ActionApplyStatusCommand,
    ActionRemoveStatusCommand,
    ActionApplySpeedModifierCommand,
    ActionSpawnEntityCommand>;

struct ActionGraphQueuedTrigger {
    CompiledActionGraphBinding binding;
    NetId self = 0;
    TriggerEvent event;
    ActionExecutionProvenance provenance;
    std::uint32_t sequence = 0;
};

struct ActionGraphCommandBatch {
    TriggerEvent event;
    ActionExecutionProvenance provenance;
    std::uint32_t sequence = 0;
    std::vector<ActionGraphCommand> commands;
};

std::optional<CompiledActionGraphBinding> compile_action_trigger_definition(
    TriggerEventType event_type,
    const KernelActionTriggerDefinition& trigger);

CompiledActionGraphBinding compile_spawn_projectile_binding(
    TriggerEventType event_type,
    std::uint32_t projectile_template_id);

CompiledActionGraphBinding compile_apply_damage_binding(
    TriggerEventType event_type,
    EntityRefSource target_source,
    std::uint16_t amount);

CompiledActionGraphBinding compile_spawn_entity_binding(
    TriggerEventType event_type,
    std::uint32_t entity_template_id,
    EventVec3Source position_source,
    EntityRefSource owner_source);

bool validate_action_graph_binding(
    const CompiledActionGraphBinding& binding,
    std::string* error);

bool evaluate_action_graph(
    const CompiledActionGraphBinding& binding,
    NetId self,
    const TriggerEvent& event,
    const ActionExecutionProvenance& provenance,
    std::vector<ActionGraphCommand>* commands,
    std::string* error);

bool dispatch_action_graph_triggers(
    std::vector<ActionGraphQueuedTrigger>* queued_triggers,
    std::vector<ActionGraphCommandBatch>* command_batches,
    std::string* error);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_ACTION_GRAPH_H_
