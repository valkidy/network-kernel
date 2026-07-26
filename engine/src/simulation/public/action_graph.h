#ifndef SIMULATION_PUBLIC_ACTION_GRAPH_H_
#define SIMULATION_PUBLIC_ACTION_GRAPH_H_

#include <cstdint>
#include <string>
#include <vector>

#include "world/public/components.h"

namespace network_example {

struct SpawnProjectileCommand {
    std::uint32_t projectile_template_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
    ActionExecutionProvenance provenance;
};

CompiledActionGraphBinding compile_spawn_projectile_binding(
    TriggerEventType event_type,
    std::uint32_t projectile_template_id);

bool validate_action_graph_binding(
    const CompiledActionGraphBinding& binding,
    std::string* error);

bool evaluate_action_graph(
    const CompiledActionGraphBinding& binding,
    NetId self,
    const TriggerEvent& event,
    const ActionExecutionProvenance& provenance,
    std::vector<SpawnProjectileCommand>* commands,
    std::string* error);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_ACTION_GRAPH_H_
