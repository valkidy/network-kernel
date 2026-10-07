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

// An apply_damage action's authored stagger: absent, or a finite non-negative
// meter value.
inline bool damage_stagger_is_authorable(
    std::uint32_t authored,
    float stagger) {
    return authored == 0u ||
        (authored == 1u && std::isfinite(stagger) && stagger >= 0.0f);
}

inline bool damage_stagger_is_authorable(const KernelActionDefinition& action) {
    return damage_stagger_is_authorable(
        action.damage_stagger_authored, action.damage_stagger);
}

// A spawn_projectile action's lifetime override and repeat, or their absence
// on any other action. Without a repeat there is nothing to scatter or
// stagger, so those must be zero too rather than quietly ignored.
inline bool spawn_repeat_is_authorable(const KernelActionDefinition& action) {
    const bool repeat_absent = action.repeat_count_max == 0u &&
        action.repeat_count_min == 0u &&
        action.repeat_scatter_radius == 0.0f &&
        action.repeat_stagger_lifetime_ticks == 0u;
    if (action.action_type != KernelEntityTriggerActionType_SpawnProjectile) {
        return repeat_absent && action.spawn_lifetime_ticks == 0u;
    }
    if (action.repeat_count_max == 0u) {
        return repeat_absent;
    }
    return action.repeat_count_min >= 1u &&
        action.repeat_count_min <= action.repeat_count_max &&
        action.repeat_count_max <= KERNEL_MAX_ACTION_REPEAT &&
        std::isfinite(action.repeat_scatter_radius) &&
        action.repeat_scatter_radius >= 0.0f;
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

// apply_suspend_movement's speeds, checked by the same three parties.
inline bool suspend_speed_is_authorable(float speed) {
    return std::isfinite(speed) && speed >= 0.0f &&
        speed <= KERNEL_MAX_SUSPEND_SPEED;
}

// The drift a suspension moves at: the horizontal of the status's direction,
// at drift_speed. A direction with no horizontal to it -- straight down, or
// none at all -- drifts nowhere.
inline glm::vec3 suspend_drift_velocity(const glm::vec3& direction, float drift_speed) {
    const float length = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    if (!(length > 0.0001f)) {
        return glm::vec3{0.0f};
    }
    return glm::vec3{
        direction.x / length * drift_speed, 0.0f, direction.z / length * drift_speed};
}

// apply_pull's numbers, checked the same way by the loader, the kernel's
// trigger validators and the command preflight.
inline bool pull_is_authorable(
    std::uint32_t mode,
    float distance,
    std::uint32_t airtime_ticks,
    float max_speed,
    float strength) {
    if (!std::isfinite(distance) || !std::isfinite(max_speed) ||
        max_speed <= 0.0f || !std::isfinite(strength) || strength <= 0.0f ||
        airtime_ticks == 0u ||
        airtime_ticks > KERNEL_MAX_IMPULSE_LOCKOUT_TICKS) {
        return false;
    }
    if (mode == KERNEL_PULL_MODE_TO_POINT) {
        return distance >= 0.0f;
    }
    return mode == KERNEL_PULL_MODE_ALONG && distance != 0.0f;
}

// A whole apply_pull action as the kernel ABI carries it: the numbers above,
// plus a point source for TO_POINT and a direction source for ALONG. One copy
// for the kernel's trigger validators, which otherwise each spell it out.
inline bool pull_action_is_authorable(const KernelActionDefinition& action) {
    if (action.target_source > KernelEntityRefSource_EventInstigator ||
        !pull_is_authorable(
            action.pull_mode,
            action.pull_distance,
            action.pull_airtime_ticks,
            action.pull_max_speed,
            action.pull_strength)) {
        return false;
    }
    if (action.pull_mode == KERNEL_PULL_MODE_TO_POINT) {
        return action.position_source == KernelEventVec3Source_Position ||
            action.position_source == KernelEventVec3Source_SubjectPosition;
    }
    return action.direction_source == KernelEventVec3Source_Direction ||
        action.direction_source == KernelEventVec3Source_SubjectDirection;
}

// Where apply_pull means to put the target, horizontally. `point` is the
// anchor for TO_POINT and a direction for ALONG. The destination keeps the
// target's own height; only the horizontal plane is steered.
inline glm::vec3 pull_destination(
    std::uint32_t mode,
    const glm::vec3& target_position,
    const glm::vec3& point,
    float distance) {
    if (mode == KERNEL_PULL_MODE_ALONG) {
        const glm::vec3 flat{point.x, 0.0f, point.z};
        const float length = std::sqrt(flat.x * flat.x + flat.z * flat.z);
        if (length <= 0.0001f) {
            return target_position;
        }
        return target_position + flat * (distance / length);
    }
    const glm::vec3 away{
        target_position.x - point.x, 0.0f, target_position.z - point.z};
    const float length = std::sqrt(away.x * away.x + away.z * away.z);
    // Standing on the anchor: gathering onto it is already done, and a ring
    // around it has no side to put the target on.
    if (length <= 0.0001f) {
        return target_position;
    }
    return glm::vec3{
        point.x + away.x * (distance / length),
        target_position.y,
        point.z + away.z * (distance / length)};
}

// The velocity that lands a target at `destination` after `airtime_ticks`.
//
// The flight is integrated semi-implicitly -- v += g*dt, then move -- so the
// height after n ticks is dt*(vy*n + g*dt*n(n+1)/2). The solver only lands a
// step that would go *below* the ground, so aiming the height at exactly zero
// on tick N (vy = -g*dt*(N+1)/2) lands it on N+1, or on N, depending on
// rounding. vy = -g*dt*(N+0.5)/2 puts the crossing half a step into tick N
// instead: still above the ground after N-1, clearly below it after N, so the
// landing tick is N whatever the rounding. Measured against the movement
// solver in apply_pull_test. Horizontally nothing acts during the flight --
// the lockout keeps the controller off it -- and the landing step still moves
// its full distance, so N ticks at v cover v*N*dt. A horizontal speed over
// max_speed is scaled down to it, which lands the target short along the same
// line rather than somewhere else.
inline glm::vec3 pull_launch_velocity(
    const glm::vec3& target_position,
    const glm::vec3& destination,
    std::uint32_t airtime_ticks,
    float fixed_delta_seconds,
    float gravity_y,
    float max_speed) {
    const float flight_seconds =
        static_cast<float>(airtime_ticks) * fixed_delta_seconds;
    glm::vec3 horizontal{
        (destination.x - target_position.x) / flight_seconds,
        0.0f,
        (destination.z - target_position.z) / flight_seconds};
    const float speed =
        std::sqrt(horizontal.x * horizontal.x + horizontal.z * horizontal.z);
    if (speed > max_speed) {
        horizontal *= max_speed / speed;
    }
    const float vertical = gravity_y < 0.0f
        ? -gravity_y * fixed_delta_seconds *
            (static_cast<float>(airtime_ticks) + 0.5f) * 0.5f
        : 0.0f;
    return glm::vec3{horizontal.x, vertical, horizontal.z};
}

struct ActionSpawnProjectileCommand {
    std::uint32_t projectile_template_id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
    ActionExecutionProvenance provenance;
    // Zero keeps the template's lifetime; extra is added either way.
    std::uint32_t lifetime_ticks = 0;
    std::uint32_t extra_lifetime_ticks = 0;
};

struct ActionApplyDamageCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint16_t amount = 0;
    ActionExecutionProvenance provenance;
    // Explicit stagger meter this hit adds; negative derives it from damage.
    float stagger = kStaggerDerivedFromDamage;
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
    // Resolved when the graph ran; zero when the action names none.
    glm::vec3 direction{0.0f};
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
    // KERNEL_SPAWN_PLACEMENT_*.
    std::uint32_t placement = 0;
};

// The destination is resolved when the command commits, from where the
// target stands then, so `point` is the anchor or direction as the event
// reported it.
struct ActionApplyPullCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t mode = KERNEL_PULL_MODE_TO_POINT;
    glm::vec3 point{0.0f};
    float distance = 0.0f;
    std::uint32_t airtime_ticks = 0;
    float max_speed = 0.0f;
    // Weighed against impulse_resistance; never scales the launch.
    float strength = 0.0f;
    ActionExecutionProvenance provenance;
};

// `source` is the building whose graph ran, `target` the actor it opens for.
struct ActionOpenUiCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t ui_id = 0;
    ActionExecutionProvenance provenance;
};

// Refills the target's active weapon at commit, from what it holds then.
struct ActionRefillWeaponReserveCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint16_t count = 0;
    std::uint16_t percent = 0;
    ActionExecutionProvenance provenance;
};

// The target rises at rise_speed and drifts at drift_velocity while status
// instance status_instance_id stands. The drift is worked out when the graph
// runs, from the status's own direction.
struct ActionApplySuspendMovementCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t status_instance_id = 0;
    float rise_speed = 0.0f;
    glm::vec3 drift_velocity{0.0f};
    ActionExecutionProvenance provenance;
};

// Nothing can strike the target while status instance status_instance_id
// stands.
struct ActionApplyUntargetableCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t status_instance_id = 0;
    ActionExecutionProvenance provenance;
};

// The target may not act while status instance status_instance_id stands.
// Only a status lifecycle batch fills that id, as for a speed modifier.
struct ActionApplyBlockActionsCommand {
    NetId source = 0;
    NetId target = 0;
    std::uint32_t status_instance_id = 0;
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
    ActionSpawnEntityCommand,
    ActionApplyPullCommand,
    ActionOpenUiCommand,
    ActionRefillWeaponReserveCommand,
    ActionApplyBlockActionsCommand,
    ActionApplySuspendMovementCommand,
    ActionApplyUntargetableCommand>;

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
