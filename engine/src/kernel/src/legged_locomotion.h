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

// Procedural legged locomotion, ported from the LocomotionTest Unity prototype
// (LeggedLocomotion.cs / ProceduralLeg.cs / TwoBoneIK.cs) to the tick-based
// kernel. Each leg holds its foot planted in world space and only takes a step
// when its "home" stance (the rest foothold under the current body pose) drifts
// past a threshold, gated on the actor actually intending to move. The gait is
// diagonally coordinated: legs sharing a gait group swing together, and a group
// may only swing while every other group is grounded.
//
// The leg solve is deliberately decoupled from the character/CharacterVirtual
// controller (design "B1"): it reads the resolved root position only as a world
// anchor for foot placement and never inspects the controller's grounded/landed
// state. The controller decides whether the body may advance; the legs react to
// the intended movement input and their own foothold raycasts.

enum class LegGaitState : std::uint8_t {
    kSupport = 0,
    kSwing = 1,
};

struct LegLocomotionState {
    std::uint32_t hip_bone_index = 0;
    std::uint32_t knee_bone_index = 0;
    std::uint32_t foot_bone_index = 0;
    std::uint32_t gait_group = 0;

    LegGaitState gait_state = LegGaitState::kSupport;
    std::uint32_t swing_tick = 0;
    bool entered_swing = false;
    bool entered_support = false;

    // Persistent world-space foot position (ProceduralLeg.currentFootPos). Held
    // still while grounded; animated along an arc while swinging. This is also
    // the IK target each tick.
    glm::vec3 foot_target_world{0.0f};
    bool foot_initialized = false;

    // Swing interpolation endpoints (world space).
    glm::vec3 swing_start_world{0.0f};
    glm::vec3 landing_target_world{0.0f};

    // Home foothold sampled this tick under the current body pose.
    glm::vec3 ground_hit_position{0.0f};
    glm::vec3 ground_hit_normal{0.0f, 1.0f, 0.0f};
    bool ground_hit_valid = false;
    std::uint32_t grounding_candidate_index = UINT32_MAX;
    std::uint32_t supporting_entity_net_id = 0;
    std::uint32_t supporting_collider_id = 0;

    // Foot position produced by the IK solve (world space), for inspection.
    glm::vec3 solved_foot_world{0.0f};
    bool ik_reach_clamped = false;
};

struct LocomotionState {
    float root_yaw_radians = 0.0f;
    // Set by advance_locomotion_state: whether the actor intends to move/turn
    // this tick (move magnitude past the deadzone, or a yaw change was applied).
    // Gates new step initiation so idle actors keep their feet planted.
    bool locomotion_active = false;

    std::vector<LegLocomotionState> legs;
    std::vector<std::uint32_t> last_processing_order;
    std::vector<KernelBoneLocalTransform> local_pose;
    bool pose_valid = false;

    // Body grounding follow output (populated by solve when body_follow_speed>0).
    // The kernel blends the transform toward these each tick; when invalid or
    // body follow is disabled the transform's height/tilt are left untouched.
    bool body_follow_valid = false;
    float body_follow_target_height = 0.0f;
    glm::vec3 body_follow_ground_normal{0.0f, 1.0f, 0.0f};
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
    float max_slope_degrees,
    float fixed_delta_seconds,
    const LocomotionGroundingQuery& grounding_query,
    LocomotionState* state);

}  // namespace network_example

#endif  // KERNEL_SRC_LEGGED_LOCOMOTION_H_
