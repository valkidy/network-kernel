#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "kernel/src/legged_locomotion.h"

namespace {

KernelSkeletonBindingDefinition make_fixture(std::uint32_t leg_count) {
    KernelSkeletonBindingDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.input_deadzone = 0.01f;
    definition.step_threshold_meters = leg_count == 2u ? 2.8f : 5.5f;
    definition.step_duration_ticks = 6u;
    definition.max_swinging_legs = 2u;
    definition.foothold_query_type = KernelFootholdQueryType_Raycast;
    definition.foothold_query_start_height_meters = 1.0f;
    definition.foothold_query_distance_meters = 2.0f;
    definition.foothold_candidate_count = 1u;
    definition.foothold_candidate_offsets[0] = KernelVec2{0.0f, 0.0f};
    definition.leg_count = leg_count;
    definition.processing_order_count = leg_count;
    const std::array<std::uint32_t, 4> groups{0u, 1u, 1u, 0u};
    const std::array<std::uint32_t, 4> order{0u, 2u, 1u, 3u};
    for (std::uint32_t index = 0u; index < leg_count; ++index) {
        definition.legs[index].leg_id = index;
        definition.legs[index].hip_bone_index = index * 3u;
        definition.legs[index].knee_bone_index = index * 3u + 1u;
        definition.legs[index].foot_bone_index = index * 3u + 2u;
        definition.legs[index].gait_group =
            leg_count == 2u ? index : groups[index];
        definition.processing_order[index] =
            leg_count == 2u ? 1u - index : order[index];
    }
    return definition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) < 0.0001f;
}

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

}  // namespace

int main() {
    const KernelSkeletonBindingDefinition quadruped = make_fixture(4u);
    require(network_example::validate_locomotion_definition(quadruped));
    KernelSkeletonBindingDefinition invalid_threshold = quadruped;
    invalid_threshold.step_threshold_meters = 0.0f;
    require(!network_example::validate_locomotion_definition(invalid_threshold));
    KernelSkeletonBindingDefinition invalid_group = quadruped;
    invalid_group.legs[0].gait_group = invalid_group.leg_count;
    require(!network_example::validate_locomotion_definition(invalid_group));
    KernelSkeletonBindingDefinition invalid_foothold = quadruped;
    invalid_foothold.foothold_query_distance_meters = 0.0f;
    require(!network_example::validate_locomotion_definition(invalid_foothold));
    network_example::LocomotionState quadruped_state;
    require(network_example::initialize_locomotion_state(
        quadruped, 0.0f, &quadruped_state));
    require(network_example::advance_locomotion_state(
        quadruped,
        KernelVec2{1.0f, 0.0f},
        90.0f,
        0.25f,
        &quadruped_state));
    require(near(quadruped_state.root_yaw_radians,
                std::numbers::pi_v<float> / 8.0f));
    for (const network_example::LegLocomotionState& leg :
         quadruped_state.legs) {
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(!leg.entered_swing);
    }
    require(quadruped_state.last_processing_order ==
           std::vector<std::uint32_t>({0u, 2u, 1u, 3u}));

    const float yaw_before_deadzone = quadruped_state.root_yaw_radians;
    require(network_example::advance_locomotion_state(
        quadruped,
        KernelVec2{0.001f, 0.001f},
        90.0f,
        0.25f,
        &quadruped_state));
    require(near(quadruped_state.root_yaw_radians, yaw_before_deadzone));

    const KernelSkeletonBindingDefinition biped = make_fixture(2u);
    network_example::LocomotionState biped_state;
    require(network_example::initialize_locomotion_state(
        biped, 0.0f, &biped_state));
    require(network_example::advance_locomotion_state(
        biped,
        KernelVec2{0.0f, 1.0f},
        45.0f,
        1.0f / 30.0f,
        &biped_state));
    require(biped_state.legs.size() == 2u);
    require(biped_state.legs[0].gait_group == 0u);
    require(biped_state.legs[1].gait_group == 1u);
    require(biped_state.last_processing_order ==
           std::vector<std::uint32_t>({1u, 0u}));
    require(near(biped_state.root_yaw_radians, 0.0f));

    network_example::LocomotionState wrap_state;
    const float degrees_to_radians = std::numbers::pi_v<float> / 180.0f;
    require(network_example::initialize_locomotion_state(
        biped, 170.0f * degrees_to_radians, &wrap_state));
    require(network_example::advance_locomotion_state(
        biped,
        KernelVec2{
            std::sin(-170.0f * degrees_to_radians),
            std::cos(-170.0f * degrees_to_radians),
        },
        5.0f,
        1.0f,
        &wrap_state));
    require(near(wrap_state.root_yaw_radians, 175.0f * degrees_to_radians));
    return 0;
}
