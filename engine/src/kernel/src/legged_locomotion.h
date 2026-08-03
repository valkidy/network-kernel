#ifndef KERNEL_SRC_LEGGED_LOCOMOTION_H_
#define KERNEL_SRC_LEGGED_LOCOMOTION_H_

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "ozz/animation/runtime/skeleton.h"

namespace network_example {

enum class LegGaitState : std::uint8_t {
    kSupport = 0,
    kSwing = 1,
};

struct LegLocomotionState {
    std::uint32_t hip_bone_index = 0;
    std::uint32_t knee_bone_index = 0;
    std::uint32_t foot_bone_index = 0;
    std::uint32_t gait_group = 0;
    std::uint32_t swing_tick = 0;
    LegGaitState gait_state = LegGaitState::kSupport;
    bool entered_swing = false;
    bool entered_support = false;
    glm::vec3 swing_start_world{0.0f};
    glm::vec3 landing_target_world{0.0f};
    glm::vec3 planted_foothold_world{0.0f};
    glm::vec3 root_position_at_plant{0.0f};
    glm::vec3 previous_hip_world{0.0f};
    glm::vec3 foot_target_world{0.0f};
    glm::vec3 solved_foot_world{0.0f};
    glm::vec3 ground_hit_position{0.0f};
    glm::vec3 ground_hit_normal{0.0f, 1.0f, 0.0f};
    std::uint32_t grounding_candidate_index = UINT32_MAX;
    std::uint32_t supporting_entity_net_id = 0;
    std::uint32_t supporting_collider_id = 0;
    bool landing_target_valid = false;
    bool planted = false;
    bool ground_hit_valid = false;
    bool ik_reach_clamped = false;
    bool foot_target_valid = false;
    bool previous_hip_world_valid = false;
};

struct LocomotionState {
    float root_yaw_radians = 0.0f;
    std::vector<LegLocomotionState> legs;
    std::vector<std::uint32_t> last_processing_order;
    std::vector<KernelBoneLocalTransform> local_pose;
    bool pose_valid = false;
};

struct LocomotionGroundingHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    std::uint32_t supporting_entity_net_id = 0;
    std::uint32_t supporting_collider_id = 0;
};

using LocomotionGroundingQuery = std::function<bool(
    const glm::vec3& origin,
    float max_distance,
    LocomotionGroundingHit* out_hit)>;

bool validate_locomotion_definition(
    const KernelSkeletonBindingDefinition& definition);

bool initialize_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    float initial_root_yaw_radians,
    LocomotionState* out_state);

bool advance_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    const KernelVec2& move_input,
    float max_yaw_degrees_per_second,
    float fixed_delta_seconds,
    LocomotionState* state);

bool solve_legged_locomotion_pose(
    const ozz::animation::Skeleton& skeleton,
    std::span<const KernelBoneLocalTransform> bind_pose,
    const KernelSkeletonBindingDefinition& definition,
    const glm::vec3& root_position,
    bool root_grounded,
    bool root_landed_this_tick,
    float max_slope_degrees,
    float fixed_delta_seconds,
    const LocomotionGroundingQuery& grounding_query,
    LocomotionState* state);

}  // namespace network_example

#endif  // KERNEL_SRC_LEGGED_LOCOMOTION_H_
