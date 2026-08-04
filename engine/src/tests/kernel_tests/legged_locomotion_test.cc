#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "kernel/src/legged_locomotion.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/skeleton_utils.h"

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
        definition.legs[index].mid_axis_local = KernelVec3{0.0f, 0.0f, 1.0f};
        definition.processing_order[index] =
            leg_count == 2u ? 1u - index : order[index];
    }
    return definition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) < 0.0001f;
}

void require_at(bool condition, int line, const char* expression) {
    if (!condition) {
        std::fprintf(
            stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_at((condition), __LINE__, #condition)

// ---------------------------------------------------------------------------
// Synthetic quadruped rig, so gait/IK behaviour can be exercised here instead of
// through a full gameplay catalog. Layout (model space, metres):
//
//   root(0,0,0) -> body(0,2,0) -> 4x hip(+-0.5, 0, +-1) -> knee -> foot
//
// Each leg is two 1 m segments with the knee nudged sideways so the bind pose is
// bent (a perfectly straight chain has no stable bend plane). The offset is on
// X, putting the limb in the XY plane so the knee hinges about Z -- the solver
// hard-codes Z as the two-bone mid axis, so a rig has to be authored to match.
// Feet therefore rest at model y = 0, i.e. on the ground plane when the root is
// at ground height, matching how the solver seats a rig.
// ---------------------------------------------------------------------------

constexpr float kSegment = 1.0f;
constexpr float kKneeOffsetX = 0.15f;
constexpr float kLegReach = 2.0f;  // hip -> foot in the bind pose

struct LegLayout {
    const char* id;
    float hip_x;
    float hip_z;
    std::uint32_t gait_group;
};

// Diagonal pairs share a gait group, as a trotting quadruped does.
constexpr std::array<LegLayout, 4> kLegLayout{{
    {"FrontLeft", 0.5f, 1.0f, 0u},
    {"RearRight", -0.5f, -1.0f, 0u},
    {"FrontRight", -0.5f, 1.0f, 1u},
    {"RearLeft", 0.5f, -1.0f, 1u},
}};

void set_joint(
    ozz::animation::offline::RawSkeleton::Joint* joint,
    const char* name,
    float x,
    float y,
    float z) {
    joint->name = name;
    joint->transform.translation = ozz::math::Float3(x, y, z);
    joint->transform.rotation = ozz::math::Quaternion::identity();
    joint->transform.scale = ozz::math::Float3(1.0f, 1.0f, 1.0f);
}

ozz::unique_ptr<ozz::animation::Skeleton> build_quadruped() {
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    auto& root = raw.roots[0];
    set_joint(&root, "root", 0.0f, 0.0f, 0.0f);
    root.children.resize(1);
    auto& body = root.children[0];
    set_joint(&body, "body", 0.0f, 2.0f, 0.0f);
    body.children.resize(kLegLayout.size());
    for (std::size_t index = 0; index < kLegLayout.size(); ++index) {
        const LegLayout& layout = kLegLayout[index];
        auto& hip = body.children[index];
        set_joint(
            &hip,
            (std::string(layout.id) + "_Hip").c_str(),
            layout.hip_x,
            0.0f,
            layout.hip_z);
        hip.children.resize(1);
        auto& knee = hip.children[0];
        set_joint(
            &knee,
            (std::string(layout.id) + "_Knee").c_str(),
            kKneeOffsetX,
            -kSegment,
            0.0f);
        knee.children.resize(1);
        set_joint(
            &knee.children[0],
            (std::string(layout.id) + "_Foot").c_str(),
            -kKneeOffsetX,
            -kSegment,
            0.0f);
    }
    ozz::animation::offline::SkeletonBuilder builder;
    return builder(raw);
}

std::vector<KernelBoneLocalTransform> make_bind_pose(
    const ozz::animation::Skeleton& skeleton) {
    std::vector<KernelBoneLocalTransform> pose;
    pose.reserve(skeleton.num_joints());
    for (int index = 0; index < skeleton.num_joints(); ++index) {
        const ozz::math::Transform rest =
            ozz::animation::GetJointLocalRestPose(skeleton, index);
        pose.push_back(KernelBoneLocalTransform{
            KernelVec3{rest.translation.x, rest.translation.y,
                       rest.translation.z},
            KernelQuat{rest.rotation.x, rest.rotation.y, rest.rotation.z,
                       rest.rotation.w},
            KernelVec3{rest.scale.x, rest.scale.y, rest.scale.z},
        });
    }
    return pose;
}

std::unordered_map<std::string, std::uint32_t> bone_indices(
    const ozz::animation::Skeleton& skeleton) {
    std::unordered_map<std::string, std::uint32_t> lookup;
    for (int index = 0; index < skeleton.num_joints(); ++index) {
        lookup[skeleton.joint_names()[index]] =
            static_cast<std::uint32_t>(index);
    }
    return lookup;
}

KernelSkeletonBindingDefinition make_rig_definition(
    const ozz::animation::Skeleton& skeleton) {
    const auto lookup = bone_indices(skeleton);
    KernelSkeletonBindingDefinition definition{};
    definition.struct_size = sizeof(definition);
    definition.bone_count = static_cast<std::uint32_t>(skeleton.num_joints());
    definition.root_bone_index = lookup.at("root");
    definition.body_bone_index = lookup.at("body");
    definition.leg_count = static_cast<std::uint32_t>(kLegLayout.size());
    definition.processing_order_count = definition.leg_count;
    definition.input_deadzone = 0.01f;
    definition.step_threshold_meters = 0.5f;
    definition.step_duration_ticks = 4u;
    definition.max_swinging_legs = 2u;
    definition.foothold_query_type = KernelFootholdQueryType_Raycast;
    definition.foothold_query_start_height_meters = 5.0f;
    definition.foothold_query_distance_meters = 20.0f;
    definition.foothold_candidate_count = 1u;
    definition.foothold_candidate_offsets[0] = KernelVec2{0.0f, 0.0f};
    for (std::uint32_t index = 0u; index < definition.leg_count; ++index) {
        const LegLayout& layout = kLegLayout[index];
        KernelSkeletonLegDefinition& leg = definition.legs[index];
        leg.leg_id = index;
        leg.hip_bone_index = lookup.at(std::string(layout.id) + "_Hip");
        leg.knee_bone_index = lookup.at(std::string(layout.id) + "_Knee");
        leg.foot_bone_index = lookup.at(std::string(layout.id) + "_Foot");
        leg.gait_group = layout.gait_group;
        leg.pole_local = KernelVec3{1.0f, 0.0f, 0.0f};
        // The limb lies in the XY plane, so the knee hinges about Z.
        leg.mid_axis_local = KernelVec3{0.0f, 0.0f, 1.0f};
        leg.step_height_meters = 0.25f;
        leg.max_reach_ratio = 0.99f;
        definition.processing_order[index] = index;
    }
    return definition;
}

// Flat ground at y = 0.
bool ground_plane(
    const glm::vec3& origin,
    float,
    network_example::LocomotionGroundingHit* hit) {
    hit->position = glm::vec3{origin.x, 0.0f, origin.z};
    hit->normal = glm::vec3{0.0f, 1.0f, 0.0f};
    return true;
}

// Ground tilted about the Z axis, rising toward +X.
network_example::LocomotionGroundingQuery sloped_ground(float slope_radians) {
    const float tangent = std::tan(slope_radians);
    return [tangent](
               const glm::vec3& origin,
               float,
               network_example::LocomotionGroundingHit* hit) {
        hit->position = glm::vec3{origin.x, origin.x * tangent, origin.z};
        hit->normal = glm::normalize(glm::vec3{-tangent, 1.0f, 0.0f});
        return true;
    };
}

// Forward kinematics of a solved local pose into model space, mirroring how the
// presentation layer composes bones (ozz orders parents before children).
std::vector<glm::vec3> model_positions(
    const std::vector<KernelBoneLocalTransform>& pose,
    const ozz::animation::Skeleton& skeleton) {
    std::vector<glm::mat4> models(pose.size());
    std::vector<glm::vec3> positions(pose.size());
    for (std::size_t index = 0; index < pose.size(); ++index) {
        const KernelBoneLocalTransform& local = pose[index];
        const glm::mat4 matrix =
            glm::translate(
                glm::mat4(1.0f),
                glm::vec3(local.local_position.x, local.local_position.y,
                          local.local_position.z)) *
            glm::mat4_cast(glm::quat(
                local.local_rotation.w, local.local_rotation.x,
                local.local_rotation.y, local.local_rotation.z)) *
            glm::scale(
                glm::mat4(1.0f),
                glm::vec3(local.local_scale.x, local.local_scale.y,
                          local.local_scale.z));
        const int parent = skeleton.joint_parents()[index];
        models[index] = parent < 0 ? matrix : models[parent] * matrix;
        positions[index] = glm::vec3(models[index][3]);
    }
    return positions;
}

// World-space bone positions exactly as a renderer would compose them: the
// entity transform (position plus the rotation the solve published) applied to
// the forward-kinematic model pose.
std::vector<glm::vec3> world_positions(
    const network_example::LocomotionState& state,
    const ozz::animation::Skeleton& skeleton,
    const glm::vec3& root_position) {
    std::vector<glm::vec3> model = model_positions(state.local_pose, skeleton);
    for (glm::vec3& position : model) {
        position = root_position + state.applied_root_rotation * position;
    }
    return model;
}

float degrees(float radians) {
    return radians * 180.0f / std::numbers::pi_v<float>;
}

float angle_between(const glm::vec3& lhs, const glm::vec3& rhs) {
    const float cosine = glm::dot(glm::normalize(lhs), glm::normalize(rhs));
    return degrees(std::acos(std::clamp(cosine, -1.0f, 1.0f)));
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

    // ---------------------------------------------------------------------
    // Behaviour on a real (synthetic) rig.
    // ---------------------------------------------------------------------
    const ozz::unique_ptr<ozz::animation::Skeleton> skeleton =
        build_quadruped();
    require(skeleton != nullptr);
    const std::vector<KernelBoneLocalTransform> bind_pose =
        make_bind_pose(*skeleton);
    const KernelSkeletonBindingDefinition rig = make_rig_definition(*skeleton);
    require(network_example::validate_locomotion_definition(rig));
    const float tick = 1.0f / 30.0f;
    const KernelVec2 forward_input{0.0f, 1.0f};

    // The rig cross-check accepts the axis this rig actually hinges about, and
    // rejects one that is merely plausible -- the failure that used to be
    // silent, since a mis-axed knee simply never bends toward its target.
    {
        std::uint32_t invalid_leg = 0u;
        require(network_example::validate_locomotion_rig(
            *skeleton, rig, &invalid_leg));
        require(invalid_leg == UINT32_MAX);

        // Sign is irrelevant: the pole picks the fold direction.
        KernelSkeletonBindingDefinition flipped = rig;
        for (std::uint32_t index = 0u; index < flipped.leg_count; ++index) {
            flipped.legs[index].mid_axis_local = KernelVec3{0.0f, 0.0f, -1.0f};
        }
        require(network_example::validate_locomotion_rig(
            *skeleton, flipped, nullptr));

        // Perpendicular to the true hinge: rejected, naming the leg.
        KernelSkeletonBindingDefinition mis_axed = rig;
        mis_axed.legs[2].mid_axis_local = KernelVec3{1.0f, 0.0f, 0.0f};
        require(!network_example::validate_locomotion_rig(
            *skeleton, mis_axed, &invalid_leg));
        require(invalid_leg == 2u);

        // A zero axis is not a usable hinge, and no longer means "assume Z".
        KernelSkeletonBindingDefinition zero_axis = rig;
        zero_axis.legs[0].mid_axis_local = KernelVec3{0.0f, 0.0f, 0.0f};
        require(!network_example::validate_locomotion_definition(zero_axis));
    }

    // Out-of-range bone indices are rejected rather than dereferenced.
    {
        KernelSkeletonBindingDefinition broken = rig;
        broken.body_bone_index = broken.bone_count;
        network_example::LocomotionState state;
        require(network_example::initialize_locomotion_state(
            broken, 0.0f, &state));
        require(!network_example::solve_legged_locomotion_pose(
            *skeleton, bind_pose, broken, glm::vec3{0.0f}, 50.0f, tick,
            ground_plane, &state));
    }

    // Feet only commit once they have ground; until then the limb keeps its
    // bind pose. With ground present they seed onto it and stay in support.
    network_example::LocomotionState walk;
    require(network_example::initialize_locomotion_state(rig, 0.0f, &walk));
    require(network_example::advance_locomotion_state(
        rig, forward_input, 90.0f, tick, &walk));
    require(network_example::solve_legged_locomotion_pose(
        *skeleton, bind_pose, rig, glm::vec3{0.0f}, 50.0f, tick,
        [](const glm::vec3&, float, network_example::LocomotionGroundingHit*) {
            return false;
        },
        &walk));
    for (const network_example::LegLocomotionState& leg : walk.legs) {
        require(!leg.foot_initialized);
        require(!leg.ground_hit_valid);
    }
    require(network_example::advance_locomotion_state(
        rig, forward_input, 90.0f, tick, &walk));
    require(network_example::solve_legged_locomotion_pose(
        *skeleton, bind_pose, rig, glm::vec3{0.0f}, 50.0f, tick, ground_plane,
        &walk));
    require(walk.pose_valid);
    for (const network_example::LegLocomotionState& leg : walk.legs) {
        require(leg.foot_initialized);
        require(leg.gait_state == network_example::LegGaitState::kSupport);
        require(near(leg.foot_target_world.y, 0.0f));
        require(!leg.ik_reach_clamped);
    }

    // With body follow off the published rotation is the bare heading and the
    // body carries no tilt.
    require(walk.body_tilt.w == 1.0f);
    require(!walk.body_follow_valid);
    require(near(
        glm::length(walk.applied_root_rotation -
                    glm::angleAxis(walk.root_yaw_radians,
                                   glm::vec3{0.0f, 1.0f, 0.0f})),
        0.0f));

    // The presentation must land each foot exactly on its target: the solve and
    // the renderer have to agree on one root transform.
    {
        const std::vector<glm::vec3> world =
            world_positions(walk, *skeleton, glm::vec3{0.0f});
        for (std::uint32_t index = 0u; index < rig.leg_count; ++index) {
            const network_example::LegLocomotionState& leg = walk.legs[index];
            require(glm::length(
                        world[rig.legs[index].foot_bone_index] -
                        leg.solved_foot_world) < 0.001f);
            require(glm::length(
                        leg.solved_foot_world - leg.foot_target_world) <
                    0.001f);
        }
    }

    // Idle: the body is shoved around but no movement is intended, so every
    // foot stays exactly where it was planted and no leg ever swings.
    {
        std::vector<glm::vec3> planted;
        for (const network_example::LegLocomotionState& leg : walk.legs) {
            planted.push_back(leg.foot_target_world);
        }
        for (std::uint32_t step = 0u; step < 120u; ++step) {
            require(network_example::advance_locomotion_state(
                rig, KernelVec2{}, 90.0f, tick, &walk));
            require(!walk.locomotion_active);
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, rig,
                glm::vec3{0.02f * static_cast<float>(step), 0.0f, 0.0f}, 50.0f,
                tick, ground_plane, &walk));
            for (std::uint32_t index = 0u; index < rig.leg_count; ++index) {
                require(!walk.legs[index].entered_swing);
                require(glm::length(
                            walk.legs[index].foot_target_world -
                            planted[index]) < 0.0001f);
            }
        }
    }

    // Walking: legs step, diagonal groups never swing together, no more than
    // max_swinging_legs are airborne, and each swing lasts exactly
    // step_duration_ticks.
    {
        network_example::LocomotionState gait;
        require(network_example::initialize_locomotion_state(rig, 0.0f, &gait));
        glm::vec3 root{0.0f};
        std::array<std::uint32_t, 4> swings{};
        std::array<std::uint32_t, 4> landings{};
        std::array<std::uint32_t, 4> airborne_run{};
        for (std::uint32_t step = 0u; step < 240u; ++step) {
            require(network_example::advance_locomotion_state(
                rig, forward_input, 90.0f, tick, &gait));
            require(gait.locomotion_active);
            root.z += 1.5f * tick;
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, rig, root, 50.0f, tick, ground_plane,
                &gait));
            std::array<bool, 2> group_swinging{};
            std::uint32_t swinging = 0u;
            for (std::uint32_t index = 0u; index < rig.leg_count; ++index) {
                const network_example::LegLocomotionState& leg =
                    gait.legs[index];
                if (leg.entered_swing) {
                    ++swings[index];
                }
                if (leg.entered_support) {
                    // A swing spans step_duration_ticks ticks of motion, the
                    // last of which lands the foot -- so an observer polling
                    // after each solve sees it airborne one tick fewer.
                    require(airborne_run[index] ==
                            rig.step_duration_ticks - 1u);
                    airborne_run[index] = 0u;
                    ++landings[index];
                }
                if (leg.gait_state == network_example::LegGaitState::kSwing) {
                    ++swinging;
                    ++airborne_run[index];
                    group_swinging[leg.gait_group] = true;
                }
            }
            require(!(group_swinging[0] && group_swinging[1]));
            require(swinging <= rig.max_swinging_legs);
        }
        for (std::uint32_t index = 0u; index < rig.leg_count; ++index) {
            // Every leg stepped repeatedly and every step it started, it
            // finished (at most one may still be mid-swing at the cut-off).
            require(swings[index] > 1u);
            require(swings[index] - landings[index] <= 1u);
        }
    }

    // A step is a frozen, one-shot commitment: both swing endpoints are set at
    // lift-off and never re-aimed, so the whole step is reproducible from the
    // single event that started it (which is what lets a remote client replay a
    // step it did not simulate). What stops freezing from landing the foot
    // behind the body is aiming at the stance the body will hold at touchdown
    // instead of the one under it now -- on flat ground at a constant velocity
    // that prediction is exact, so the foot lands right where a per-tick
    // retarget would have put it.
    //
    // The swing is stretched here so the drift a naive freeze would suffer is
    // larger than the step threshold itself, i.e. bigger than the whole signal
    // the gait runs on. Both cases are checked: translating, and turning in
    // place (a turning body sweeps its stance without moving its root at all).
    {
        KernelSkeletonBindingDefinition long_swing = rig;
        long_swing.step_duration_ticks = 12u;

        const auto stance_under = [&](
            const network_example::LocomotionState& state,
            const glm::vec3& root,
            std::uint32_t index) {
            const glm::vec3 foot_model{
                kLegLayout[index].hip_x, 0.0f, kLegLayout[index].hip_z};
            const glm::vec3 world =
                root + state.applied_root_rotation * foot_model;
            return glm::vec3{world.x, 0.0f, world.z};  // ground_plane is y = 0
        };

        // Translating forward at a constant 1.5 m/s.
        {
            network_example::LocomotionState gait;
            require(network_example::initialize_locomotion_state(
                long_swing, 0.0f, &gait));
            const float advance_per_tick = 1.5f * tick;
            const float uncompensated_lag = advance_per_tick *
                static_cast<float>(long_swing.step_duration_ticks - 1u);
            require(uncompensated_lag > long_swing.step_threshold_meters);

            glm::vec3 root{0.0f};
            std::array<glm::vec3, 4> frozen{};
            std::array<bool, 4> swinging{};
            std::uint32_t landings = 0u;
            for (std::uint32_t step = 0u; step < 240u; ++step) {
                require(network_example::advance_locomotion_state(
                    long_swing, forward_input, 90.0f, tick, &gait));
                root.z += advance_per_tick;
                require(network_example::solve_legged_locomotion_pose(
                    *skeleton, bind_pose, long_swing, root, 50.0f, tick,
                    ground_plane, &gait));
                for (std::uint32_t index = 0u;
                     index < long_swing.leg_count;
                     ++index) {
                    const network_example::LegLocomotionState& leg =
                        gait.legs[index];
                    if (leg.entered_swing) {
                        frozen[index] = leg.landing_target_world;
                        swinging[index] = true;
                    } else if (swinging[index]) {
                        require(leg.landing_target_world == frozen[index]);
                    }
                    if (leg.entered_support) {
                        swinging[index] = false;
                        ++landings;
                        require(glm::length(
                                    leg.foot_target_world -
                                    stance_under(gait, root, index)) <
                                uncompensated_lag * 0.02f);
                    }
                }
            }
            require(landings > 4u);
        }

        // Turning in place: the root never moves, but the stance sweeps around
        // it, so the landing spot has to be extrapolated through the rotation.
        {
            network_example::LocomotionState gait;
            require(network_example::initialize_locomotion_state(
                long_swing, 0.0f, &gait));
            const float yaw_per_tick = 0.05f;
            const glm::vec3 root{0.0f};
            // Arc a foot sweeps over one swing, at the rig's stance radius.
            const float stance_radius = std::sqrt(
                kLegLayout[0].hip_x * kLegLayout[0].hip_x +
                kLegLayout[0].hip_z * kLegLayout[0].hip_z);
            const float uncompensated_lag = yaw_per_tick * stance_radius *
                static_cast<float>(long_swing.step_duration_ticks - 1u);
            require(uncompensated_lag > long_swing.step_threshold_meters);

            std::array<glm::vec3, 4> frozen{};
            std::array<bool, 4> swinging{};
            std::uint32_t landings = 0u;
            for (std::uint32_t step = 0u; step < 240u; ++step) {
                gait.root_yaw_radians += yaw_per_tick;
                require(network_example::advance_locomotion_state(
                    long_swing, KernelVec2{}, 90.0f, tick, &gait));
                require(network_example::solve_legged_locomotion_pose(
                    *skeleton, bind_pose, long_swing, root, 50.0f, tick,
                    ground_plane, &gait));
                for (std::uint32_t index = 0u;
                     index < long_swing.leg_count;
                     ++index) {
                    const network_example::LegLocomotionState& leg =
                        gait.legs[index];
                    if (leg.entered_swing) {
                        frozen[index] = leg.landing_target_world;
                        swinging[index] = true;
                    } else if (swinging[index]) {
                        require(leg.landing_target_world == frozen[index]);
                    }
                    if (leg.entered_support) {
                        swinging[index] = false;
                        ++landings;
                        require(glm::length(
                                    leg.foot_target_world -
                                    stance_under(gait, root, index)) <
                                uncompensated_lag * 0.02f);
                    }
                }
            }
            require(landings > 4u);
        }
    }

    // Turning in place: no translation input at all, the heading is written
    // straight into the state between ticks. The legs must still notice.
    {
        network_example::LocomotionState turn;
        require(network_example::initialize_locomotion_state(rig, 0.0f, &turn));
        require(network_example::advance_locomotion_state(
            rig, KernelVec2{}, 90.0f, tick, &turn));
        require(network_example::solve_legged_locomotion_pose(
            *skeleton, bind_pose, rig, glm::vec3{0.0f}, 50.0f, tick,
            ground_plane, &turn));
        bool stepped = false;
        for (std::uint32_t step = 0u; step < 60u && !stepped; ++step) {
            turn.root_yaw_radians += 0.05f;  // external facing change
            require(network_example::advance_locomotion_state(
                rig, KernelVec2{}, 90.0f, tick, &turn));
            require(turn.locomotion_active);
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, rig, glm::vec3{0.0f}, 50.0f, tick,
                ground_plane, &turn));
            for (const network_example::LegLocomotionState& leg : turn.legs) {
                stepped = stepped || leg.entered_swing;
            }
        }
        require(stepped);
    }

    // A foot stranded beyond the leg's reach recovers: it steps back to a
    // foothold it can stand on instead of hanging there clamped forever.
    {
        network_example::LocomotionState stranded;
        require(network_example::initialize_locomotion_state(
            rig, 0.0f, &stranded));
        require(network_example::advance_locomotion_state(
            rig, forward_input, 90.0f, tick, &stranded));
        require(network_example::solve_legged_locomotion_pose(
            *skeleton, bind_pose, rig, glm::vec3{0.0f}, 50.0f, tick,
            ground_plane, &stranded));
        // Teleport the body well beyond a leg length; every planted foot is now
        // far out of reach.
        const glm::vec3 far_root{0.0f, 0.0f, 4.0f * kLegReach};
        require(network_example::advance_locomotion_state(
            rig, forward_input, 90.0f, tick, &stranded));
        require(network_example::solve_legged_locomotion_pose(
            *skeleton, bind_pose, rig, far_root, 50.0f, tick, ground_plane,
            &stranded));
        bool recovered = false;
        for (std::uint32_t step = 0u; step < 60u && !recovered; ++step) {
            require(network_example::advance_locomotion_state(
                rig, forward_input, 90.0f, tick, &stranded));
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, rig, far_root, 50.0f, tick, ground_plane,
                &stranded));
            recovered = true;
            for (const network_example::LegLocomotionState& leg :
                 stranded.legs) {
                recovered = recovered && !leg.ik_reach_clamped;
            }
        }
        require(recovered);
    }

    // Body follow on a slope: the tilt has to keep accumulating until the body
    // actually matches the ground, not stall at one smoothing step's worth of
    // it, and the body bone must not sweep sideways while it happens.
    {
        const float slope = 20.0f * degrees_to_radians;
        KernelSkeletonBindingDefinition tilted = rig;
        tilted.body_follow_speed = 10.0f;
        tilted.slope_alignment = 1.0f;
        const network_example::LocomotionGroundingQuery ground =
            sloped_ground(slope);

        network_example::LocomotionState body;
        require(network_example::initialize_locomotion_state(
            tilted, 0.0f, &body));
        glm::vec3 root{0.0f};
        float first_tilt = 0.0f;
        for (std::uint32_t step = 0u; step < 120u; ++step) {
            require(network_example::advance_locomotion_state(
                tilted, KernelVec2{}, 90.0f, tick, &body));
            if (body.body_follow_valid) {
                root.y += (body.body_follow_target_height - root.y) *
                    (1.0f - std::exp(-tilted.body_follow_speed * tick));
            }
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, tilted, root, 50.0f, tick, ground,
                &body));
            require(body.pose_valid);
            const glm::vec3 up =
                body.applied_root_rotation * glm::vec3{0.0f, 1.0f, 0.0f};
            if (step == 1u) {
                first_tilt = angle_between(up, glm::vec3{0.0f, 1.0f, 0.0f});
            }
        }
        require(body.body_follow_valid);
        const glm::vec3 body_up =
            body.applied_root_rotation * glm::vec3{0.0f, 1.0f, 0.0f};
        const glm::vec3 slope_normal =
            glm::normalize(glm::vec3{-std::tan(slope), 1.0f, 0.0f});
        // Converged onto the slope rather than parked at a fraction of it.
        require(angle_between(body_up, slope_normal) < 1.0f);
        require(first_tilt < degrees(slope) * 0.5f);

        // The tilt pivots about the body bone, so the body stays over the
        // entity origin instead of swinging out by the rig's height.
        const std::vector<glm::vec3> world =
            world_positions(body, *skeleton, root);
        const glm::vec3 body_world = world[tilted.body_bone_index];
        require(std::abs(body_world.x - root.x) < 0.001f);
        require(std::abs(body_world.z - root.z) < 0.001f);

        // And the renderer still reproduces the solve exactly under tilt: a
        // rotation mismatch between the two would show up here as sliding feet.
        for (std::uint32_t index = 0u; index < tilted.leg_count; ++index) {
            const network_example::LegLocomotionState& leg = body.legs[index];
            require(glm::length(
                        world[tilted.legs[index].foot_bone_index] -
                        leg.solved_foot_world) < 0.001f);
            if (!leg.ik_reach_clamped) {
                require(glm::length(
                            leg.solved_foot_world - leg.foot_target_world) <
                        0.001f);
            }
        }
    }
    return 0;
}
