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

// Where one of the rig's per-bone colliders sits after a solve, expressed
// relative to the ROOT TRANSFORM the caller passed in -- not relative to the
// skeleton root, which the solve seats vertically by an offset of its own. So
// the world box is exactly
//
//     world_position = transform.position + transform.rotation * local_position
//     world_rotation = transform.rotation * local_rotation
//
// and nothing else has to know how the pose was seated. This holds only while
// transform.rotation is the rotation the solve was given, which is why a rig
// carrying colliders is refused slope_alignment > 0 at catalog load: tilt makes
// those two rotations differ on the authority and not on a follower.
//
// local_rotation is orthonormal. ozz composes each bone's rest scale into its
// model matrix, and on a GEO_ bone that scale IS the box's size -- so it must
// be divided out here and taken from the bind pose as half extents instead.
struct SolvedColliderPose {
    glm::vec3 local_position{0.0f};
    glm::quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
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
    // Yaw still owed toward the heading advance is slewing to, and whether
    // there is such a heading at all. A turn stops when it arrives, so the
    // solve must not extrapolate rotation past it; a yaw written from outside
    // has no destination to bound it, hence the flag.
    float pending_yaw_radians = 0.0f;
    bool has_pending_yaw_radians = false;

    std::vector<LegLocomotionState> legs;
    std::vector<std::uint32_t> last_processing_order;
    std::vector<KernelBoneLocalTransform> local_pose;
    bool pose_valid = false;

    // One entry per KernelSkeletonBindingDefinition::colliders, in that order.
    // Empty for a rig that declares none, and emptied whenever a solve fails,
    // so a caller that materialises physics bodies from it can never publish a
    // stale limb. Filled by the same model-space refresh the feet are read from,
    // which costs nothing extra.
    std::vector<SolvedColliderPose> solved_collider_poses;

    // Smoothed tilt of the body onto the terrain, accumulated across ticks and
    // identity while body follow is disabled. It lives here, not on the entity
    // transform, because the leg solve and the presentation must agree on one
    // root rotation: solve places the feet using applied_root_rotation, so the
    // caller MUST write that same rotation to the transform or the feet slide by
    // the tilt. (Re-deriving a pure-yaw rotation on the transform each tick also
    // restarts the smoothing, so the tilt would never converge.)
    glm::quat body_tilt{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat applied_root_rotation{1.0f, 0.0f, 0.0f, 0.0f};

    // Terrain-fit body height for this tick, valid only when body follow is on
    // and at least one foot is planted. Applying it stays the caller's job.
    bool body_follow_valid = false;
    float body_follow_target_height = 0.0f;

    // The height the caller last actually applied, and whether it did. This is
    // what the caller must smooth FROM, rather than from the transform: the
    // movement controller writes its own answer into the transform earlier in
    // the same tick, and blending against that would readmit the body-vs-feet
    // mismatch that body follow exists to remove. Cleared whenever the
    // controller takes height back, so the next hand-over starts from wherever
    // the body actually is instead of from a stale value.
    bool has_body_follow_applied_height = false;
    float body_follow_applied_height = 0.0f;

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

// A step exactly as the authoritative solve committed it, and the whole of what
// a follower needs to reproduce the swing. Both endpoints of a swing are frozen
// at lift-off (see LegLocomotionState), so this is a complete description of the
// step rather than a sample of one.
//
// The lift-off position is deliberately absent. A follower that is in sync
// already holds it -- it is where that foot is planted -- and a follower that
// is not converges anyway, because the landing position is absolute world
// space rather than a delta. That is also why a lost step is self-healing: the
// leg is wrong only until its next step, not forever.
struct LocomotionStepEvent {
    std::uint32_t leg_index = 0;
    std::uint32_t start_tick = 0;
    glm::vec3 landing_target_world{0.0f};
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

// Cross-checks the authored limb colliders against the rig they hang on. The
// load-bearing check is that a limb's bone does not rest at unit scale: these
// rigs express a limb's dimensions AS the bone's rest scale, so a unit-scaled
// bone means the geometry lives in mesh vertices instead, where this derivation
// cannot see it -- and the collider would silently become a 1m cube rather than
// fail. On failure the offending index is written to out_invalid_limb_index
// when it is not null.
bool validate_locomotion_colliders(
    std::span<const KernelBoneLocalTransform> bind_pose,
    const KernelSkeletonBindingDefinition& definition,
    std::uint32_t* out_invalid_limb_index);

// The half extents a limb collider takes from its bone, i.e. half the bone's
// rest scale. Spheres use the x component as a radius. Kept beside the
// validation so the runtime and the check cannot disagree about what the
// authored bone means.
glm::vec3 locomotion_collider_half_extents(
    std::span<const KernelBoneLocalTransform> bind_pose,
    const KernelSkeletonColliderDefinition& limb);

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

// ---------------------------------------------------------------------------
// Follower path. A follower reproduces an authoritative actor's legs from its
// replicated root plus the steps it is told about -- it never grounds itself
// and never decides to step. That is what lets a remote client show the same
// legs the server is using for collision without replicating 41 bones, and it
// is strictly cheaper than the authoritative path: no foothold raycasts at all,
// so no dependence on the client's terrain query agreeing with the server's.
// ---------------------------------------------------------------------------

// Reads out the steps the last authoritative solve committed, which are exactly
// the legs whose entered_swing is set. Returns the number of steps found, which
// may exceed out_events.size(); only the first out_events.size() are written.
std::uint32_t collect_locomotion_step_events(
    const LocomotionState& state,
    std::uint32_t solve_tick,
    std::span<LocomotionStepEvent> out_events);

// Applies a replicated step to a follower state, to be called before the solve
// for current_tick. A step that arrives late resumes mid-arc rather than
// restarting, so a delayed event costs the beginning of one swing instead of
// desynchronising the leg; one that arrives after its swing would have ended
// lands the foot immediately.
bool apply_locomotion_step_event(
    const KernelSkeletonBindingDefinition& definition,
    const LocomotionStepEvent& event,
    std::uint32_t current_tick,
    LocomotionState* state);

// Plants a follower's foot without stepping it: the baseline a follower needs
// when it first sees an actor, and the resync for the one case a lost step does
// not heal on its own (a loss immediately before the actor goes idle, which
// produces no further steps to correct it).
bool set_locomotion_foot_anchor(
    const KernelSkeletonBindingDefinition& definition,
    std::uint32_t leg_index,
    const glm::vec3& world_position,
    LocomotionState* state);

// Solves the pose from the follower's own leg state alone. The caller must have
// written the replicated heading into state->root_yaw_radians first, since a
// follower reads its heading off the replicated transform instead of deriving
// it from movement input; there is no advance_locomotion_state on this path.
//
// entered_swing stays false here -- it marks steps this state decided, and a
// follower decides none. entered_support still fires on touchdown.
//
// Body follow is not applied here, and today that costs nothing even though the
// authority runs it (every rig on this path authors body_follow_speed 10). Its
// two halves replicate differently. The height is applied by the caller to the
// entity transform, which IS replicated, so a follower inherits it through the
// root instead of refitting it -- which it could not do anyway, having sampled
// no footholds. The tilt is the half that would diverge, since it needs the
// ground normals under the feet; but it is a separate opt-in gated on
// slope_alignment > 0, and every rig here authors 0, so the tilt stays identity
// on both sides. Raising slope_alignment would mean replicating the normals or
// accepting a divergence worth the tilt times the stance radius -- metres on a
// rig whose feet sit ten of them out from the body.
bool solve_legged_locomotion_follower_pose(
    const ozz::animation::Skeleton& skeleton,
    std::span<const KernelBoneLocalTransform> bind_pose,
    const KernelSkeletonBindingDefinition& definition,
    const glm::vec3& root_position,
    float fixed_delta_seconds,
    LocomotionState* state);

}  // namespace network_example

#endif  // KERNEL_SRC_LEGGED_LOCOMOTION_H_
