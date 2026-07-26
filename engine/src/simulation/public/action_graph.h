#ifndef SIMULATION_PUBLIC_ACTION_GRAPH_H_
#define SIMULATION_PUBLIC_ACTION_GRAPH_H_

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

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

using ActionGraphCommand = std::variant<
    ActionSpawnProjectileCommand,
    ActionApplyDamageCommand>;

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

CompiledActionGraphBinding compile_spawn_projectile_binding(
    TriggerEventType event_type,
    std::uint32_t projectile_template_id);

CompiledActionGraphBinding compile_apply_damage_binding(
    TriggerEventType event_type,
    EntityRefSource target_source,
    std::uint16_t amount);

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
