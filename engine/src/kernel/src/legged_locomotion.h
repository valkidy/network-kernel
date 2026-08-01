#ifndef KERNEL_SRC_LEGGED_LOCOMOTION_H_
#define KERNEL_SRC_LEGGED_LOCOMOTION_H_

#include <cstdint>
#include <vector>

#include "kernel/public/kernel_types.h"

namespace network_example {

enum class LegGaitState : std::uint8_t {
    kSupport = 0,
    kSwing = 1,
};

struct LegLocomotionState {
    std::uint32_t hip_bone_index = 0;
    std::uint32_t knee_bone_index = 0;
    std::uint32_t foot_bone_index = 0;
    std::uint32_t phase_tick = 0;
    LegGaitState gait_state = LegGaitState::kSupport;
};

struct LocomotionState {
    float root_yaw_radians = 0.0f;
    std::uint32_t gait_start_tick = 0;
    std::uint32_t gait_phase_tick = 0;
    std::vector<LegLocomotionState> legs;
    std::vector<std::uint32_t> last_processing_order;
};

bool validate_locomotion_definition(
    const KernelSkeletonBindingDefinition& definition);

bool initialize_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    float initial_root_yaw_radians,
    std::uint32_t simulation_tick,
    LocomotionState* out_state);

bool advance_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    const KernelVec2& move_input,
    float max_yaw_degrees_per_second,
    float fixed_delta_seconds,
    std::uint32_t simulation_tick,
    LocomotionState* state);

}  // namespace network_example

#endif  // KERNEL_SRC_LEGGED_LOCOMOTION_H_
