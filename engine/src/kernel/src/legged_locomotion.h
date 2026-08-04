#ifndef KERNEL_SRC_LEGGED_LOCOMOTION_H_
#define KERNEL_SRC_LEGGED_LOCOMOTION_H_

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "kernel/public/kernel_types.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"

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

    // Swing interpolation endpoints (world space). Both are frozen when the
    // swing begins and held for the whole step, so a step is fully described by
    // one event: (leg, start tick, start position, landing position). That is
    // what lets a remote client reproduce the step from a small replicated
    // message instead of re-deriving the landing spot every tick from a root
    // position that lags the server's by the interpolation delay.
    //
    // The prototype re-aimed the landing spot every frame
    // (ProceduralLeg.Tick: `stepTargetPos = grounded`). Freezing alone would
    // land the foot behind the stance by however far the body travels during a
    // swing -- a systematic backward bias, and one that grows with the gait's
    // duty factor. It is cancelled by aiming the frozen target at where the
    // stance will be at touchdown rather than where it is at lift-off, so the
    // two agree whenever the body holds its velocity through the step.
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
    // Yaw as of the end of the previous advance. advance compares it against
    // root_yaw_radians so a heading change from ANY source counts as activity:
    // this tick's move input, or an external write between ticks (AI facing, or
    // a turn-in-place that carries no translation input).
    float last_advanced_yaw_radians = 0.0f;
    // Set by advance_locomotion_state: whether the actor intends to move or turn
    // this tick. Gates new step initiation so idle actors keep their feet planted.
    bool locomotion_active = false;

    // Yaw travelled during the last advance. Kept because advance collapses
    // last_advanced_yaw_radians onto root_yaw_radians before solve runs, and
    // solve needs the rate to aim a step at the stance the body will hold when
    // the foot lands (a turning body sweeps its stance by yaw_rate * reach,
    // which is metres on a long-legged rig).
    float last_yaw_delta_radians = 0.0f;

    std::vector<LegLocomotionState> legs;
    std::vector<std::uint32_t> last_processing_order;
    std::vector<KernelBoneLocalTransform> local_pose;
    bool pose_valid = false;

    // Smoothed tilt of the body onto the terrain, accumulated across ticks and
    // identity while body follow is disabled. It lives here, not on the entity
    // transform, because the leg solve and the presentation must agree on one
    // root rotation: solve places the feet using applied_root_rotation, so the
    // caller MUST write that same rotation to the transform or the feet slide by
    // the tilt. (Re-deriving a pure-yaw rotation on the transform each tick also
    // restarts the smoothing, so the tilt would never converge.)
    glm::quat body_tilt{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat applied_root_rotation{1.0f, 0.0f, 0.0f, 0.0f};

    // Terrain-fit body height for this tick, valid only when body follow is on.
    // Applying it stays the caller's job: the character controller owns position.
    bool body_follow_valid = false;
    float body_follow_target_height = 0.0f;

    // Root position handed to the previous solve. Its delta is the body's
    // translation over one tick, which is how far the stance drifts per tick;
    // solve extrapolates it over a swing to place the landing target. Derived
    // here instead of taken as a parameter so the solve stays a function of the
    // root positions it is already given.
    glm::vec3 last_root_position{0.0f};
    bool has_last_root_position = false;

    // Solver scratch, kept alive between ticks purely to stop the per-entity
    // pose solve from heap-allocating on every tick. Contents carry no meaning
    // across calls.
    std::vector<ozz::math::SoaTransform> scratch_locals;
    std::vector<ozz::math::Float4x4> scratch_models;
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

// Cross-checks the authored legs against the rig they claim to drive: each
// leg's mid_axis_local must be roughly parallel to the hinge axis its bind pose
// actually implies, cross(knee - hip, foot - knee). Catches the failure that is
// otherwise silent -- a rig whose knees bend about a different axis, where the
// two-bone solve simply cannot reach its target. On failure the offending leg
// index is written to out_invalid_leg_index when it is not null.
bool validate_locomotion_rig(
    const ozz::animation::Skeleton& skeleton,
    const KernelSkeletonBindingDefinition& definition,
    std::uint32_t* out_invalid_leg_index);

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
