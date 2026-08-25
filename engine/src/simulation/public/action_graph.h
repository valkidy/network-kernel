#ifndef SIMULATION_PUBLIC_ACTION_GRAPH_H_
#define SIMULATION_PUBLIC_ACTION_GRAPH_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "world/public/components.h"

namespace network_example {

// apply_impulse's three questions -- is this authored coherently, how strongly
// does it push, and what does it add to the velocity -- answered in one place.
// kernel.cc holds three separate copies of the trigger validator and systems.cc
// a fourth; letting each spell out the strength rule on its own is how the
// loader and the kernel drifted apart twice before.
inline bool impulse_strength_is_authorable(
    std::uint32_t strength_mode,
    float horizontal,
    float vertical) {
    if (!std::isfinite(horizontal)) {
        return false;
    }
    if (strength_mode != KERNEL_IMPULSE_STRENGTH_MODE_SPLIT) {
        return horizontal > 0.0f;
    }
    // Split form allows a zero horizontal, which is what a pure vertical
    // launch is; what it cannot be is zero on both axes.
    return std::isfinite(vertical) && horizontal >= 0.0f &&
        (horizontal > 0.0f || vertical != 0.0f);
}

// The magnitude an impulse is weighed at against a target's
// impulse_resistance. Radial mode returns the strength unchanged -- bit for
// bit -- so no existing template's resistance outcome can move.
inline float impulse_effective_strength(
    std::uint32_t strength_mode,
    float horizontal,
    float vertical) {
    return strength_mode == KERNEL_IMPULSE_STRENGTH_MODE_SPLIT
        ? std::max(horizontal, std::fabs(vertical))
        : horizontal;
}

// What the impulse adds to the target's velocity. `direction` must already be
// normalised. Split mode treats vertical as an absolute signed Y increment
// rather than a scale on direction.y, because an area effect's direction is
// radial from the blast: for a level blast direction.y is ~0, and scaling
// zero can never launch anything upward.
inline glm::vec3 impulse_velocity_delta(
    std::uint32_t strength_mode,
    const glm::vec3& direction,
    float horizontal,
    float vertical) {
    return strength_mode == KERNEL_IMPULSE_STRENGTH_MODE_SPLIT
        ? glm::vec3{direction.x * horizontal, vertical, direction.z * horizontal}
        : direction * horizontal;
}

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
    std::uint32_t lockout_ticks = 0;
    std::uint32_t strength_mode = KERNEL_IMPULSE_STRENGTH_MODE_RADIAL;
    float vertical_strength = 0.0f;
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
