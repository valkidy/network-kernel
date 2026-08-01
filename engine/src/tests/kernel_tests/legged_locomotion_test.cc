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
    definition.gait_cycle_ticks = leg_count == 2u ? 20u : 60u;
    definition.gait_swing_ticks = leg_count == 2u ? 5u : 24u;
    definition.max_swinging_legs = 2u;
    definition.foothold_query_type = KernelFootholdQueryType_Raycast;
    definition.foothold_query_start_height_meters = 1.0f;
    definition.foothold_query_distance_meters = 2.0f;
    definition.foothold_candidate_count = 1u;
    definition.foothold_candidate_offsets[0] = KernelVec2{0.0f, 0.0f};
    definition.leg_count = leg_count;
    definition.processing_order_count = leg_count;
    const std::array<std::uint32_t, 4> offsets{0u, 15u, 30u, 45u};
    const std::array<std::uint32_t, 4> order{0u, 2u, 1u, 3u};
    for (std::uint32_t index = 0u; index < leg_count; ++index) {
        definition.legs[index].leg_id = index;
        definition.legs[index].hip_bone_index = index * 3u;
        definition.legs[index].knee_bone_index = index * 3u + 1u;
        definition.legs[index].foot_bone_index = index * 3u + 2u;
        definition.legs[index].phase_offset_ticks =
            leg_count == 2u ? index * 10u : offsets[index];
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
    KernelSkeletonBindingDefinition invalid_schedule = quadruped;
    invalid_schedule.max_swinging_legs = 1u;
    require(!network_example::validate_locomotion_definition(invalid_schedule));
    KernelSkeletonBindingDefinition invalid_foothold = quadruped;
    invalid_foothold.foothold_query_distance_meters = 0.0f;
    require(!network_example::validate_locomotion_definition(invalid_foothold));
    network_example::LocomotionState quadruped_state;
    require(network_example::initialize_locomotion_state(
        quadruped, 0.0f, 100u, &quadruped_state));
    require(network_example::advance_locomotion_state(
        quadruped,
        KernelVec2{1.0f, 0.0f},
        90.0f,
        0.25f,
        100u,
        &quadruped_state));
    require(near(quadruped_state.root_yaw_radians,
                std::numbers::pi_v<float> / 8.0f));
    require(quadruped_state.gait_phase_tick == 0u);
    require(quadruped_state.legs[0].gait_state ==
           network_example::LegGaitState::kSwing);
    require(quadruped_state.legs[1].gait_state ==
           network_example::LegGaitState::kSwing);
    require(quadruped_state.legs[2].gait_state ==
           network_example::LegGaitState::kSupport);
    require(quadruped_state.last_processing_order ==
           std::vector<std::uint32_t>({0u, 2u, 1u, 3u}));

    const float yaw_before_deadzone = quadruped_state.root_yaw_radians;
    require(network_example::advance_locomotion_state(
        quadruped,
        KernelVec2{0.001f, 0.001f},
        90.0f,
        0.25f,
        101u,
        &quadruped_state));
    require(near(quadruped_state.root_yaw_radians, yaw_before_deadzone));

    const KernelSkeletonBindingDefinition biped = make_fixture(2u);
    network_example::LocomotionState biped_state;
    require(network_example::initialize_locomotion_state(
        biped, 0.0f, 10u, &biped_state));
    require(network_example::advance_locomotion_state(
        biped,
        KernelVec2{0.0f, 1.0f},
        45.0f,
        1.0f / 30.0f,
        17u,
        &biped_state));
    require(biped_state.legs.size() == 2u);
    require(biped_state.gait_phase_tick == 7u);
    require(biped_state.legs[0].phase_tick == 7u);
    require(biped_state.legs[1].phase_tick == 17u);
    require(biped_state.last_processing_order ==
           std::vector<std::uint32_t>({1u, 0u}));
    require(near(biped_state.root_yaw_radians, 0.0f));

    network_example::LocomotionState wrap_state;
    const float degrees_to_radians = std::numbers::pi_v<float> / 180.0f;
    require(network_example::initialize_locomotion_state(
        biped, 170.0f * degrees_to_radians, 20u, &wrap_state));
    require(network_example::advance_locomotion_state(
        biped,
        KernelVec2{
            std::sin(-170.0f * degrees_to_radians),
            std::cos(-170.0f * degrees_to_radians),
        },
        5.0f,
        1.0f,
        20u,
        &wrap_state));
    require(near(wrap_state.root_yaw_radians, 175.0f * degrees_to_radians));
    return 0;
}
