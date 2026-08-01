#include "kernel/src/legged_locomotion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace network_example {
namespace {

bool valid_definition(const KernelSkeletonBindingDefinition& definition) {
    if (definition.struct_size < sizeof(KernelSkeletonBindingDefinition) ||
        definition.leg_count == 0u ||
        definition.leg_count > KERNEL_MAX_SKELETON_LEGS ||
        definition.processing_order_count != definition.leg_count ||
        definition.gait_cycle_ticks == 0u ||
        definition.gait_swing_ticks == 0u ||
        definition.gait_swing_ticks >= definition.gait_cycle_ticks ||
        definition.max_swinging_legs == 0u ||
        definition.max_swinging_legs > definition.leg_count ||
        !std::isfinite(definition.input_deadzone) ||
        definition.input_deadzone < 0.0f ||
        definition.input_deadzone >= 1.0f) {
        return false;
    }
    std::array<bool, KERNEL_MAX_SKELETON_LEGS> ordered{};
    for (std::uint32_t order = 0u; order < definition.leg_count; ++order) {
        const std::uint32_t leg_index = definition.processing_order[order];
        if (leg_index >= definition.leg_count || ordered[leg_index]) {
            return false;
        }
        ordered[leg_index] = true;
    }
    std::uint32_t authored_max_swinging = 0u;
    for (std::uint32_t origin = 0u; origin < definition.leg_count; ++origin) {
        const std::uint32_t origin_offset =
            definition.legs[origin].phase_offset_ticks %
            definition.gait_cycle_ticks;
        const std::uint32_t gait_phase =
            (definition.gait_cycle_ticks - origin_offset) %
            definition.gait_cycle_ticks;
        std::uint32_t swinging = 0u;
        for (std::uint32_t leg = 0u; leg < definition.leg_count; ++leg) {
            const std::uint32_t leg_phase = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(gait_phase) +
                 definition.legs[leg].phase_offset_ticks %
                     definition.gait_cycle_ticks) %
                definition.gait_cycle_ticks);
            swinging += leg_phase < definition.gait_swing_ticks ? 1u : 0u;
        }
        authored_max_swinging = std::max(authored_max_swinging, swinging);
    }
    if (authored_max_swinging > definition.max_swinging_legs) {
        return false;
    }
    return true;
}

float shortest_angle_delta(float from, float to) {
    return std::remainder(
        to - from,
        2.0f * std::numbers::pi_v<float>);
}

}  // namespace

bool validate_locomotion_definition(
    const KernelSkeletonBindingDefinition& definition) {
    return valid_definition(definition);
}

bool initialize_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    float initial_root_yaw_radians,
    std::uint32_t simulation_tick,
    LocomotionState* out_state) {
    if (out_state == nullptr || !valid_definition(definition) ||
        !std::isfinite(initial_root_yaw_radians)) {
        return false;
    }
    LocomotionState state;
    state.root_yaw_radians = initial_root_yaw_radians;
    state.gait_start_tick = simulation_tick;
    state.legs.reserve(definition.leg_count);
    state.last_processing_order.reserve(definition.leg_count);
    for (std::uint32_t index = 0u; index < definition.leg_count; ++index) {
        const KernelSkeletonLegDefinition& leg = definition.legs[index];
        state.legs.push_back(LegLocomotionState{
            leg.hip_bone_index,
            leg.knee_bone_index,
            leg.foot_bone_index,
            0u,
            LegGaitState::kSupport,
        });
    }
    *out_state = std::move(state);
    return true;
}

bool advance_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    const KernelVec2& move_input,
    float max_yaw_degrees_per_second,
    float fixed_delta_seconds,
    std::uint32_t simulation_tick,
    LocomotionState* state) {
    if (state == nullptr || !valid_definition(definition) ||
        state->legs.size() != definition.leg_count ||
        !std::isfinite(move_input.x) || !std::isfinite(move_input.y) ||
        !std::isfinite(max_yaw_degrees_per_second) ||
        max_yaw_degrees_per_second <= 0.0f ||
        !std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0f) {
        return false;
    }

    const float move_magnitude_squared =
        move_input.x * move_input.x + move_input.y * move_input.y;
    if (move_magnitude_squared >=
        definition.input_deadzone * definition.input_deadzone) {
        const float target_yaw = std::atan2(move_input.x, move_input.y);
        const float max_yaw_step = max_yaw_degrees_per_second *
            std::numbers::pi_v<float> / 180.0f * fixed_delta_seconds;
        state->root_yaw_radians += std::clamp(
            shortest_angle_delta(state->root_yaw_radians, target_yaw),
            -max_yaw_step,
            max_yaw_step);
        state->root_yaw_radians = std::remainder(
            state->root_yaw_radians,
            2.0f * std::numbers::pi_v<float>);
    }

    state->gait_phase_tick =
        (simulation_tick - state->gait_start_tick) %
        definition.gait_cycle_ticks;
    state->last_processing_order.clear();
    for (std::uint32_t order = 0u; order < definition.leg_count; ++order) {
        const std::uint32_t leg_index = definition.processing_order[order];
        const KernelSkeletonLegDefinition& definition_leg =
            definition.legs[leg_index];
        LegLocomotionState& leg = state->legs[leg_index];
        leg.phase_tick = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(state->gait_phase_tick) +
             definition_leg.phase_offset_ticks) %
            definition.gait_cycle_ticks);
        leg.gait_state = leg.phase_tick < definition.gait_swing_ticks
            ? LegGaitState::kSwing
            : LegGaitState::kSupport;
        state->last_processing_order.push_back(leg_index);
    }
    return true;
}

}  // namespace network_example
