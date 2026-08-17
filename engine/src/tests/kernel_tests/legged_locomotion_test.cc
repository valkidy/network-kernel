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

// Scale of the optional GEO_ bone below. Chosen to match quadruped_actor's real
// GEO_Leg0_Lower, so the half extents the solve hands out are the rig's own
// numbers rather than round ones.
constexpr float kGeoScaleX = 1.5f;
constexpr float kGeoScaleY = 19.0f;

// with_geometry_bones appends one scaled, zero-offset child to every foot,
// standing in for the GEO_ bones the real rigs carry colliders on. Zero offset
// is what makes the collider's solved position directly comparable to the
// foot's, and the non-unit scale is what makes the scale-divided-out rotation
// observable: a matrix whose columns are 1.5 / 19 / 1.5 long does not cast to a
// unit quaternion unless the scale really was removed first.
ozz::unique_ptr<ozz::animation::Skeleton> build_quadruped(
    bool with_geometry_bones = false) {
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
        auto& foot = knee.children[0];
        set_joint(
            &foot,
            (std::string(layout.id) + "_Foot").c_str(),
            -kKneeOffsetX,
            -kSegment,
            0.0f);
        if (!with_geometry_bones) {
            continue;
        }
        foot.children.resize(1);
        auto& geometry = foot.children[0];
        set_joint(
            &geometry,
            (std::string("GEO_") + layout.id).c_str(),
            0.0f,
            0.0f,
            0.0f);
        geometry.transform.scale =
            ozz::math::Float3(kGeoScaleX, kGeoScaleY, kGeoScaleX);
    }
    ozz::animation::offline::SkeletonBuilder builder;
    return builder(raw);
}

// The same quadruped, bone-for-bone identical in model space, but with every
// knee yawed 90 degrees about Y and its foot's local offset counter-rotated to
// put the foot back where it was. The limbs therefore bend in exactly the same
// plane as build_quadruped()'s, while the knee's own frame no longer coincides
// with the rig's -- which is the only condition under which reading mid_axis in
// the wrong frame produces a different answer.
ozz::unique_ptr<ozz::animation::Skeleton> build_yawed_knee_quadruped() {
    // Right-handed yaw of +90 deg about Y maps (x, y, z) -> (z, y, -x).
    const ozz::math::Quaternion knee_yaw =
        ozz::math::Quaternion::FromAxisAngle(
            ozz::math::Float3(0.0f, 1.0f, 0.0f),
            std::numbers::pi_v<float> / 2.0f);

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
        knee.transform.rotation = knee_yaw;
        knee.children.resize(1);
        // Inverse yaw of the original (-kKneeOffsetX, -kSegment, 0):
        // Ry(-90) maps (x, y, z) -> (-z, y, x).
        set_joint(
            &knee.children[0],
            (std::string(layout.id) + "_Foot").c_str(),
            0.0f,
            -kSegment,
            -kKneeOffsetX);
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

// Rolling terrain. The follower equivalence tests run on this rather than a
// plane so that a follower cannot appear to agree merely because every foothold
// has the same height.
network_example::LocomotionGroundingQuery undulating_ground() {
    return [](const glm::vec3& origin,
              float,
              network_example::LocomotionGroundingHit* hit) {
        constexpr float kWaveNumber = 0.9f;
        constexpr float kAmplitude = 0.3f;
        hit->position = glm::vec3{
            origin.x,
            kAmplitude * (std::sin(origin.x * kWaveNumber) +
                          std::cos(origin.z * kWaveNumber)),
            origin.z};
        hit->normal = glm::normalize(glm::vec3{
            -kAmplitude * kWaveNumber * std::cos(origin.x * kWaveNumber),
            1.0f,
            kAmplitude * kWaveNumber * std::sin(origin.z * kWaveNumber)});
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

    // The axis is read in the knee's own frame, not the rig's. Every joint of
    // build_quadruped() is identity-rotated, so the two frames coincide there
    // and the distinction is invisible; a rig whose knees carry a real rotation
    // is the only thing that pins it down. This is the shape of every rig built
    // by authoring one limb and re-aiming duplicates: the legs are exact copies
    // with one knee-local axis between them, while their model-space hinges
    // fan out with the hips.
    {
        const ozz::unique_ptr<ozz::animation::Skeleton> yawed =
            build_yawed_knee_quadruped();
        require(yawed != nullptr);
        KernelSkeletonBindingDefinition yawed_rig = make_rig_definition(*yawed);

        // The limbs bend in the same plane as build_quadruped()'s, so the hinge
        // is still model-space Z; yawing the knee 90 degrees about Y puts that
        // same hinge on X in the knee's frame. One value covers the whole rig.
        for (std::uint32_t index = 0u; index < yawed_rig.leg_count; ++index) {
            yawed_rig.legs[index].mid_axis_local = KernelVec3{1.0f, 0.0f, 0.0f};
        }
        std::uint32_t invalid_leg = 0u;
        require(network_example::validate_locomotion_rig(
            *yawed, yawed_rig, &invalid_leg));
        require(invalid_leg == UINT32_MAX);

        // Model-space Z is what the old check demanded and what ozz would then
        // have bent about in the wrong plane; it is now correctly rejected.
        KernelSkeletonBindingDefinition model_space_axis = yawed_rig;
        for (std::uint32_t index = 0u;
             index < model_space_axis.leg_count;
             ++index) {
            model_space_axis.legs[index].mid_axis_local =
                KernelVec3{0.0f, 0.0f, 1.0f};
        }
        require(!network_example::validate_locomotion_rig(
            *yawed, model_space_axis, nullptr));
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

    // Follower equivalence. An actor told only where its steps land -- given no
    // terrain query and no gait decision of its own -- must reproduce the
    // authority's legs. This is the load-bearing assumption behind replicating
    // steps to a remote client instead of replicating 41 bones, so it is checked
    // against the real solve, on rolling terrain, along a curving path.
    {
        const network_example::LocomotionGroundingQuery ground =
            undulating_ground();
        constexpr std::uint32_t kTicks = 400u;
        constexpr float kSpeed = 1.5f;
        constexpr float kTurnRate = 0.35f;  // radians per second
        constexpr std::uint32_t kNoDrop = 0xFFFFFFFFu;
        constexpr float kEpsilon = 0.0001f;

        struct PendingStep {
            network_example::LocomotionStepEvent event;
            std::uint32_t deliver_tick;
        };
        struct Trace {
            std::uint32_t steps = 0u;
            std::uint32_t compared_ticks = 0u;
            std::uint32_t planted_comparisons = 0u;
            float worst_planted_error = 0.0f;
            float worst_any_error = 0.0f;
            float worst_pose_error = 0.0f;
            std::uint32_t drop_tick = 0u;
            std::uint32_t heal_tick = 0u;
            bool dropped = false;
            bool healed = false;
        };

        const auto run = [&](std::uint32_t delay_ticks,
                             std::uint32_t drop_step_index) {
            Trace trace;
            network_example::LocomotionState authority;
            network_example::LocomotionState follower;
            require(network_example::initialize_locomotion_state(
                rig, 0.0f, &authority));
            require(network_example::initialize_locomotion_state(
                rig, 0.0f, &follower));
            std::vector<PendingStep> pending;
            glm::vec3 root{0.0f};
            bool seeded = false;

            for (std::uint32_t t = 0u; t < kTicks; ++t) {
                const float heading =
                    kTurnRate * static_cast<float>(t) * tick;
                require(network_example::advance_locomotion_state(
                    rig,
                    KernelVec2{std::sin(heading), std::cos(heading)},
                    90.0f,
                    tick,
                    &authority));
                root += glm::vec3{
                    std::sin(authority.root_yaw_radians),
                    0.0f,
                    std::cos(authority.root_yaw_radians)} * (kSpeed * tick);
                require(network_example::solve_legged_locomotion_pose(
                    *skeleton, bind_pose, rig, root, 50.0f, tick, ground,
                    &authority));

                std::array<network_example::LocomotionStepEvent, 4> emitted{};
                const std::uint32_t emitted_count =
                    network_example::collect_locomotion_step_events(
                        authority, t, emitted);
                require(emitted_count <= emitted.size());
                for (std::uint32_t index = 0u; index < emitted_count; ++index) {
                    const bool dropped = trace.steps == drop_step_index;
                    ++trace.steps;
                    if (dropped) {
                        trace.drop_tick = t;
                        trace.dropped = true;
                        continue;
                    }
                    pending.push_back({emitted[index], t + delay_ticks});
                }

                // Relevance-enter baseline: the follower is handed the feet once
                // and keeps up on steps alone from there.
                if (!seeded) {
                    bool all_planted = true;
                    for (const network_example::LegLocomotionState& leg :
                         authority.legs) {
                        all_planted = all_planted && leg.foot_initialized;
                    }
                    if (!all_planted) {
                        continue;
                    }
                    for (std::uint32_t index = 0u;
                         index < rig.leg_count;
                         ++index) {
                        require(network_example::set_locomotion_foot_anchor(
                            rig,
                            index,
                            authority.legs[index].foot_target_world,
                            &follower));
                    }
                    seeded = true;
                    continue;
                }

                // A follower reads its heading off the replicated transform
                // rather than deriving it from input it does not have.
                follower.root_yaw_radians = authority.root_yaw_radians;
                for (std::size_t index = 0u; index < pending.size();) {
                    if (pending[index].deliver_tick != t) {
                        ++index;
                        continue;
                    }
                    require(network_example::apply_locomotion_step_event(
                        rig, pending[index].event, t, &follower));
                    pending.erase(pending.begin() +
                                  static_cast<std::ptrdiff_t>(index));
                }
                require(network_example::solve_legged_locomotion_follower_pose(
                    *skeleton, bind_pose, rig, root, tick, &follower));
                require(follower.pose_valid);
                ++trace.compared_ticks;

                bool all_agree = true;
                for (std::uint32_t index = 0u; index < rig.leg_count; ++index) {
                    const network_example::LegLocomotionState& mine =
                        follower.legs[index];
                    const network_example::LegLocomotionState& theirs =
                        authority.legs[index];
                    const float error = glm::length(
                        mine.foot_target_world - theirs.foot_target_world);
                    trace.worst_any_error =
                        std::max(trace.worst_any_error, error);
                    all_agree = all_agree && error <= kEpsilon;
                    // A foot both sides consider planted is the case that has
                    // to agree exactly: it is what a collider would sit on and
                    // what the eye reads as "the foot is not sliding". Swing
                    // windows are allowed to differ while a late step catches up.
                    if (mine.gait_state ==
                            network_example::LegGaitState::kSupport &&
                        theirs.gait_state ==
                            network_example::LegGaitState::kSupport) {
                        ++trace.planted_comparisons;
                        // For the dropped-step run the whole point is that the
                        // follower is briefly wrong, so only the recovered
                        // stretch is held to exact agreement.
                        if (drop_step_index == kNoDrop || trace.healed) {
                            trace.worst_planted_error =
                                std::max(trace.worst_planted_error, error);
                        }
                    }
                    trace.worst_pose_error = std::max(
                        trace.worst_pose_error,
                        glm::length(mine.solved_foot_world -
                                    theirs.solved_foot_world));
                }
                if (trace.dropped && !trace.healed && t > trace.drop_tick &&
                    all_agree) {
                    trace.heal_tick = t;
                    trace.healed = true;
                }
            }
            return trace;
        };

        // In sync: every foot, every tick, bit-for-bit. Note the follower is
        // never told where a step LIFTS OFF from -- it uses its own planted
        // position -- so this also proves that half of the event is redundant
        // and does not need replicating.
        {
            const Trace trace = run(0u, kNoDrop);
            require(trace.steps > 20u);
            require(trace.compared_ticks > kTicks / 2u);
            require(trace.planted_comparisons > 100u);
            require(trace.worst_any_error < kEpsilon);
            require(trace.worst_pose_error < kEpsilon);
        }

        // Steps delivered three ticks late: the follower starts each swing
        // behind and catches up mid-arc, so the airborne foot genuinely differs
        // (asserted, so this case cannot pass vacuously) -- but every foot both
        // sides consider planted still agrees exactly.
        {
            const Trace trace = run(3u, kNoDrop);
            require(trace.steps > 20u);
            require(trace.worst_any_error > 0.01f);
            require(trace.worst_planted_error < kEpsilon);
        }

        // A step dropped outright. The landing position is absolute, not a
        // delta, so the leg is wrong only until its next step rather than
        // forever: the follower must converge again without any resync.
        {
            const Trace trace = run(0u, 8u);
            require(trace.steps > 8u);
            require(trace.dropped);
            require(trace.healed);
            require(trace.heal_tick > trace.drop_tick);
            // Well inside one step cycle for this rig (~14 ticks).
            require(trace.heal_tick - trace.drop_tick < 40u);
            // And it stays converged for the rest of the run.
            require(trace.worst_planted_error < kEpsilon);
        }
    }

    // The same equivalence, but for the thing that actually goes into the
    // physics world: the per-bone collider frames. Feet agreeing is necessary
    // and not sufficient -- a collider also carries an orientation, and that
    // one comes out of the IK'd knee and foot basis rather than out of the
    // replicated landing position. If the two sides can disagree anywhere, it
    // is there.
    //
    // This is the whole argument for spending zero snapshot bytes on limbs, so
    // it is measured under packet loss rather than only in the clean case.
    {
        const ozz::unique_ptr<ozz::animation::Skeleton> geometry_skeleton =
            build_quadruped(/*with_geometry_bones=*/true);
        require(geometry_skeleton != nullptr);
        const std::vector<KernelBoneLocalTransform> geometry_bind_pose =
            make_bind_pose(*geometry_skeleton);
        const auto geometry_lookup = bone_indices(*geometry_skeleton);
        KernelSkeletonBindingDefinition geometry_rig =
            make_rig_definition(*geometry_skeleton);
        geometry_rig.collider_count = geometry_rig.leg_count;
        for (std::uint32_t index = 0u; index < geometry_rig.leg_count; ++index) {
            KernelSkeletonColliderDefinition& collider =
                geometry_rig.colliders[index];
            collider.bone_index =
                geometry_lookup.at(std::string("GEO_") + kLegLayout[index].id);
            collider.leg_index = index;
            collider.shape_type = KernelColliderShapeType_OrientedBox;
            collider.purpose_flags = KernelColliderPurpose_Limb;
            collider.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
        }

        const network_example::LocomotionGroundingQuery ground =
            undulating_ground();
        constexpr std::uint32_t kTicks = 400u;
        constexpr float kSpeed = 1.5f;
        constexpr float kTurnRate = 0.35f;
        constexpr float kEpsilon = 0.0001f;

        // Exactly how the kernel composes a limb collider: the root the solve
        // published, not the entity's current transform. See make_limb_collider.
        const auto limb_world = [](const network_example::LocomotionState& state,
                                   std::uint32_t index) {
            return state.last_root_position +
                state.applied_root_rotation *
                    state.solved_collider_poses[index].local_position;
        };
        const auto limb_rotation =
            [](const network_example::LocomotionState& state,
               std::uint32_t index) {
                return state.applied_root_rotation *
                    state.solved_collider_poses[index].local_rotation;
            };

        struct LimbTrace {
            std::uint32_t steps = 0u;
            std::uint32_t dropped_steps = 0u;
            std::uint32_t compared_ticks = 0u;
            std::uint32_t agreeing_ticks = 0u;
            float worst_position_error = 0.0f;
            float worst_rotation_chord = 0.0f;
        };

        // Deterministic loss, so a failure is reproducible rather than a story
        // about one unlucky run.
        const auto run = [&](std::uint32_t loss_per_thousand) {
            LimbTrace trace;
            std::uint32_t rng = 0x9e3779b9u;
            network_example::LocomotionState authority;
            network_example::LocomotionState follower;
            require(network_example::initialize_locomotion_state(
                geometry_rig, 0.0f, &authority));
            require(network_example::initialize_locomotion_state(
                geometry_rig, 0.0f, &follower));
            std::vector<network_example::LocomotionStepEvent> pending;
            glm::vec3 root{0.0f};
            bool seeded = false;

            for (std::uint32_t t = 0u; t < kTicks; ++t) {
                const float heading = kTurnRate * static_cast<float>(t) * tick;
                require(network_example::advance_locomotion_state(
                    geometry_rig,
                    KernelVec2{std::sin(heading), std::cos(heading)},
                    90.0f,
                    tick,
                    &authority));
                root += glm::vec3{
                    std::sin(authority.root_yaw_radians),
                    0.0f,
                    std::cos(authority.root_yaw_radians)} * (kSpeed * tick);
                require(network_example::solve_legged_locomotion_pose(
                    *geometry_skeleton, geometry_bind_pose, geometry_rig, root,
                    50.0f, tick, ground, &authority));

                std::array<network_example::LocomotionStepEvent, 4> emitted{};
                const std::uint32_t emitted_count =
                    network_example::collect_locomotion_step_events(
                        authority, t, emitted);
                for (std::uint32_t index = 0u; index < emitted_count; ++index) {
                    ++trace.steps;
                    rng = rng * 1664525u + 1013904223u;
                    if ((rng >> 16u) % 1000u < loss_per_thousand) {
                        ++trace.dropped_steps;
                        continue;
                    }
                    pending.push_back(emitted[index]);
                }

                if (!seeded) {
                    bool all_planted = true;
                    for (const network_example::LegLocomotionState& leg :
                         authority.legs) {
                        all_planted = all_planted && leg.foot_initialized;
                    }
                    if (!all_planted) {
                        continue;
                    }
                    for (std::uint32_t index = 0u;
                         index < geometry_rig.leg_count;
                         ++index) {
                        require(network_example::set_locomotion_foot_anchor(
                            geometry_rig,
                            index,
                            authority.legs[index].foot_target_world,
                            &follower));
                    }
                    seeded = true;
                    continue;
                }

                follower.root_yaw_radians = authority.root_yaw_radians;
                for (const network_example::LocomotionStepEvent& event :
                     pending) {
                    require(network_example::apply_locomotion_step_event(
                        geometry_rig, event, t, &follower));
                }
                pending.clear();
                require(network_example::solve_legged_locomotion_follower_pose(
                    *geometry_skeleton, geometry_bind_pose, geometry_rig, root,
                    tick, &follower));
                require(follower.pose_valid);

                // Both sides must publish a full set, or there is nothing to
                // compare and the comparison would pass by being empty.
                require(authority.solved_collider_poses.size() ==
                        geometry_rig.collider_count);
                require(follower.solved_collider_poses.size() ==
                        geometry_rig.collider_count);
                ++trace.compared_ticks;

                float tick_position_error = 0.0f;
                float tick_rotation_chord = 0.0f;
                for (std::uint32_t index = 0u;
                     index < geometry_rig.collider_count;
                     ++index) {
                    tick_position_error = std::max(
                        tick_position_error,
                        glm::length(
                            limb_world(follower, index) -
                            limb_world(authority, index)));
                    // Chord distance between the two orientations, taken the
                    // short way round because q and -q are the same rotation.
                    // Deliberately not acos(dot): near dot = 1 that turns the
                    // last bit of a float into a milliradian, and these
                    // quaternions agree to the bit.
                    const glm::quat mine = limb_rotation(follower, index);
                    const glm::quat theirs = limb_rotation(authority, index);
                    const float chord = std::min(
                        glm::length(mine - theirs), glm::length(mine + theirs));
                    tick_rotation_chord =
                        std::max(tick_rotation_chord, chord);
                }
                trace.worst_position_error = std::max(
                    trace.worst_position_error, tick_position_error);
                trace.worst_rotation_chord = std::max(
                    trace.worst_rotation_chord, tick_rotation_chord);
                if (tick_position_error < kEpsilon &&
                    tick_rotation_chord < kEpsilon) {
                    ++trace.agreeing_ticks;
                }
            }
            return trace;
        };

        // Lossless: the client's limbs ARE the server's limbs. Position and
        // orientation, every collider, every tick. This is the assertion the
        // "0 bytes" answer rests on.
        {
            const LimbTrace trace = run(0u);
            require(trace.steps > 20u);
            require(trace.dropped_steps == 0u);
            require(trace.compared_ticks > kTicks / 2u);
            require(trace.agreeing_ticks == trace.compared_ticks);
            require(trace.worst_position_error < kEpsilon);
            require(trace.worst_rotation_chord < kEpsilon);
        }

        // Under loss the limbs go wrong, but only on the legs whose step was
        // lost and only until those legs step again. Both rates assert a real
        // divergence as well as a bound, so neither can pass by the loss having
        // quietly done nothing.
        //
        // 5% is the pessimistic end of what this prototype's unreliable step
        // channel should ever see; 20% is there to show the shape of the curve.
        const LimbTrace light_loss = run(50u);
        const LimbTrace heavy_loss = run(200u);
        require(light_loss.dropped_steps > 0u);
        require(heavy_loss.dropped_steps > light_loss.dropped_steps * 2u);
        require(light_loss.worst_position_error > kEpsilon);
        // Bounded by roughly one stride, because a leg that missed its step
        // stays at its old plant rather than drifting: the landing target is
        // absolute world space, not a delta.
        require(light_loss.worst_position_error < 2.0f * kSpeed);
        require(heavy_loss.worst_position_error < 2.0f * kSpeed);
        // The load-bearing property: four times the losses does NOT mean four
        // times the error. Nothing accumulates, so the worst case is set by how
        // far one leg can be behind, not by how many steps went missing.
        require(heavy_loss.worst_position_error <
                light_loss.worst_position_error * 3.0f);
        // And most ticks are still exact even while legs are missing steps,
        // because only the legs that lost one are wrong.
        require(light_loss.agreeing_ticks * 10u >
                light_loss.compared_ticks * 7u);
        require(heavy_loss.agreeing_ticks * 10u >
                heavy_loss.compared_ticks * 3u);
    }

    // A turn that finishes before the foot lands must not throw the step past
    // where the actor was ever going. The extrapolation assumes the body holds
    // its motion for the whole swing, which translation does and a turn does
    // not: a turn stops on arrival, and a turn is the same order as a swing, so
    // a step begun mid-turn would otherwise aim at rotation that is already
    // over -- on a wide stance that lands the foot further away than the step
    // was worth.
    {
        KernelSkeletonBindingDefinition long_swing = rig;
        long_swing.step_duration_ticks = 18u;
        network_example::LocomotionState turning;
        require(network_example::initialize_locomotion_state(
            long_swing, 0.0f, &turning));

        // Walk straight until the legs are planted and stepping.
        glm::vec3 root{0.0f};
        for (std::uint32_t step = 0u; step < 40u; ++step) {
            require(network_example::advance_locomotion_state(
                long_swing, forward_input, 90.0f, tick, &turning));
            root.z += 1.5f * tick;
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, long_swing, root, 50.0f, tick,
                ground_plane, &turning));
        }

        // Alternate the heading target by 20 degrees every 10 ticks. Each turn
        // completes in about 7 ticks at 90 deg/s -- well inside an 18 tick
        // swing -- and turns come often enough that steps reliably begin during
        // one, which is the case that goes wrong. The heading itself never
        // leaves a 20 degree band however long this runs.
        const float turn_radians = 20.0f * degrees_to_radians;
        float worst_overshoot = 0.0f;
        for (std::uint32_t step = 0u; step < 240u; ++step) {
            const float heading =
                (step / 10u) % 2u == 0u ? turn_radians : 0.0f;
            const KernelVec2 turned_input{
                std::sin(heading), std::cos(heading)};
            require(network_example::advance_locomotion_state(
                long_swing, turned_input, 90.0f, tick, &turning));
            root += glm::vec3{
                std::sin(turning.root_yaw_radians),
                0.0f,
                std::cos(turning.root_yaw_radians)} * (1.5f * tick);
            require(network_example::solve_legged_locomotion_pose(
                *skeleton, bind_pose, long_swing, root, 50.0f, tick,
                ground_plane, &turning));
            for (std::uint32_t index = 0u;
                 index < long_swing.leg_count;
                 ++index) {
                const network_example::LegLocomotionState& leg =
                    turning.legs[index];
                if (!leg.entered_swing || !leg.ground_hit_valid) {
                    continue;
                }
                // ground_hit_position is this leg's stance under the body at
                // lift-off, so this is exactly how far ahead the step is aimed.
                const float lead = glm::length(
                    leg.landing_target_world - leg.ground_hit_position);
                worst_overshoot = std::max(worst_overshoot, lead);
            }
        }
        // The heading only ever moves 20 degrees, so no step may be aimed
        // further than the stance travels over one swing at that heading
        // change plus the walk itself. Uncapped rotation extrapolation
        // reaches well past this.
        const float stance_radius = std::sqrt(
            kLegLayout[0].hip_x * kLegLayout[0].hip_x +
            kLegLayout[0].hip_z * kLegLayout[0].hip_z);
        const float turn_arc = turn_radians * stance_radius;
        const float walk_lead = 1.5f * tick *
            static_cast<float>(long_swing.step_duration_ticks - 1u);
        require(worst_overshoot > 0.0f);
        require(worst_overshoot <= turn_arc + walk_lead + 0.001f);
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

    // The authored crouch seats the body exactly that far below where body
    // follow would otherwise settle it, and buys the knees that much reach.
    // Measured on flat ground so the only difference between the two runs is
    // the parameter.
    {
        const network_example::LocomotionGroundingQuery ground =
            sloped_ground(0.0f);
        const auto settle = [&](float crouch, float* out_height) {
            KernelSkeletonBindingDefinition crouched = rig;
            crouched.body_follow_speed = 10.0f;
            crouched.slope_alignment = 0.0f;
            crouched.stance_crouch_meters = crouch;
            network_example::LocomotionState body;
            require(network_example::initialize_locomotion_state(
                crouched, 0.0f, &body));
            glm::vec3 root{0.0f};
            float longest = 0.0f;
            for (std::uint32_t step = 0u; step < 240u; ++step) {
                require(network_example::advance_locomotion_state(
                    crouched, KernelVec2{}, 90.0f, tick, &body));
                if (body.body_follow_valid) {
                    root.y += (body.body_follow_target_height - root.y) *
                        (1.0f - std::exp(-crouched.body_follow_speed * tick));
                }
                require(network_example::solve_legged_locomotion_pose(
                    *skeleton, bind_pose, crouched, root, 50.0f, tick, ground,
                    &body));
            }
            require(body.body_follow_valid);
            *out_height = root.y;
            const std::vector<glm::vec3> world =
                world_positions(body, *skeleton, root);
            for (std::uint32_t index = 0u; index < crouched.leg_count; ++index) {
                const KernelSkeletonLegDefinition& leg = crouched.legs[index];
                longest = std::max(
                    longest,
                    glm::length(
                        world[leg.foot_bone_index] -
                        world[leg.hip_bone_index]));
            }
            return longest;
        };
        float upright_height = 0.0f;
        float crouched_height = 0.0f;
        const float upright_reach = settle(0.0f, &upright_height);
        const float crouched_reach = settle(1.5f, &crouched_height);
        require(std::abs(
            (upright_height - crouched_height) - 1.5f) < 0.01f);
        // Crouching pulls the feet in toward the hips, which is the whole point:
        // the slack it frees is what a foothold below the stance spends.
        require(crouched_reach < upright_reach - 0.1f);
    }

    {
        // Limb colliders take their size from the bone's rest scale, which is
        // the only place the rigs record it. Everything here is about that one
        // derivation and the ways it must refuse to guess.
        const auto make_bind_pose = [](const KernelVec3& limb_scale) {
            std::vector<KernelBoneLocalTransform> bind_pose(4u);
            for (KernelBoneLocalTransform& bone : bind_pose) {
                bone.local_rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
                bone.local_scale = KernelVec3{1.0f, 1.0f, 1.0f};
            }
            bind_pose[3].local_scale = limb_scale;
            return bind_pose;
        };
        const auto make_definition = [](std::uint8_t shape_type) {
            KernelSkeletonBindingDefinition definition = make_fixture(1u);
            definition.bone_count = 4u;
            definition.collider_count = 1u;
            definition.colliders[0].bone_index = 3u;
            definition.colliders[0].leg_index = 0u;
            definition.colliders[0].shape_type = shape_type;
            definition.colliders[0].purpose_flags =
                KernelColliderPurpose_Limb | KernelColliderPurpose_Hit;
            definition.colliders[0].layer_mask =
                KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
            return definition;
        };

        // The rigs author a limb as a unit primitive scaled by the bone, so the
        // rest scale is the box's FULL extents -- half of it is the half extent.
        // This is the number quadruped_actor's GEO_Leg0_Lower actually carries.
        const std::vector<KernelBoneLocalTransform> bind_pose =
            make_bind_pose(KernelVec3{1.5f, 19.0f, 1.5f});
        const KernelSkeletonBindingDefinition definition =
            make_definition(KernelColliderShapeType_OrientedBox);
        std::uint32_t invalid_limb = 0u;
        require(network_example::validate_locomotion_colliders(
            bind_pose, definition, &invalid_limb));
        require(invalid_limb == UINT32_MAX);
        const glm::vec3 half_extents = network_example::locomotion_collider_half_extents(
            bind_pose, definition.colliders[0]);
        require(near(half_extents.x, 0.75f));
        require(near(half_extents.y, 9.5f));
        require(near(half_extents.z, 0.75f));

        // A bone at unit scale carries no dimensions: the rig baked them into
        // mesh vertices, which never reach the kernel. Accepting it would
        // register a plausible-looking 1m cube, so it must fail instead.
        const std::vector<KernelBoneLocalTransform> unit_pose =
            make_bind_pose(KernelVec3{1.0f, 1.0f, 1.0f});
        require(!network_example::validate_locomotion_colliders(
            unit_pose, definition, &invalid_limb));
        require(invalid_limb == 0u);

        // A sphere has one radius, so an unevenly scaled bone cannot be one --
        // picking an axis would silently resize it.
        const KernelSkeletonBindingDefinition sphere_definition =
            make_definition(KernelColliderShapeType_Sphere);
        require(!network_example::validate_locomotion_colliders(
            bind_pose, sphere_definition, &invalid_limb));
        const std::vector<KernelBoneLocalTransform> uniform_pose =
            make_bind_pose(KernelVec3{8.0f, 8.0f, 8.0f});
        require(network_example::validate_locomotion_colliders(
            uniform_pose, sphere_definition, &invalid_limb));

        // Out-of-range bone, and a leg this rig does not have.
        KernelSkeletonBindingDefinition bad_bone = definition;
        bad_bone.colliders[0].bone_index = 9u;
        require(!network_example::validate_locomotion_colliders(
            bind_pose, bad_bone, &invalid_limb));
        KernelSkeletonBindingDefinition bad_leg = definition;
        bad_leg.colliders[0].leg_index = 5u;
        require(!network_example::validate_locomotion_colliders(
            bind_pose, bad_leg, &invalid_limb));
        // KERNEL_MAX_SKELETON_LEGS is the sentinel for "not part of a leg",
        // which the biped's arms rely on, so it must stay legal.
        KernelSkeletonBindingDefinition no_leg = definition;
        no_leg.colliders[0].leg_index = KERNEL_MAX_SKELETON_LEGS;
        require(network_example::validate_locomotion_colliders(
            bind_pose, no_leg, &invalid_limb));

        // Two colliders on one bone would occupy the same frame, and the
        // runtime keys them by bone.
        KernelSkeletonBindingDefinition duplicate = definition;
        duplicate.collider_count = 2u;
        duplicate.colliders[1] = duplicate.colliders[0];
        require(!network_example::validate_locomotion_colliders(
            bind_pose, duplicate, &invalid_limb));
        require(invalid_limb == 1u);
    }

    // ---------------------------------------------------------------------
    // What the solve publishes for those colliders.
    // ---------------------------------------------------------------------
    {
        const ozz::unique_ptr<ozz::animation::Skeleton> geometry_skeleton =
            build_quadruped(/*with_geometry_bones=*/true);
        require(geometry_skeleton != nullptr);
        const std::vector<KernelBoneLocalTransform> geometry_bind_pose =
            make_bind_pose(*geometry_skeleton);
        const auto geometry_lookup = bone_indices(*geometry_skeleton);
        KernelSkeletonBindingDefinition geometry_rig =
            make_rig_definition(*geometry_skeleton);
        geometry_rig.collider_count = geometry_rig.leg_count;
        for (std::uint32_t index = 0u; index < geometry_rig.leg_count; ++index) {
            KernelSkeletonColliderDefinition& collider =
                geometry_rig.colliders[index];
            collider.bone_index =
                geometry_lookup.at(std::string("GEO_") + kLegLayout[index].id);
            collider.leg_index = index;
            collider.shape_type = KernelColliderShapeType_OrientedBox;
            collider.purpose_flags = KernelColliderPurpose_Limb;
            collider.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
        }
        std::uint32_t invalid_limb = 0u;
        require(network_example::validate_locomotion_colliders(
            geometry_bind_pose, geometry_rig, &invalid_limb));

        // Walk far enough that the IK is actually bending the limbs, so the
        // collider frames below are not just the bind pose read back.
        const glm::vec3 root{3.0f, 0.0f, -2.0f};
        network_example::LocomotionState geometry_state;
        require(network_example::initialize_locomotion_state(
            geometry_rig, 0.7f, &geometry_state));
        for (std::uint32_t step = 0u; step < 12u; ++step) {
            require(network_example::advance_locomotion_state(
                geometry_rig, forward_input, 90.0f, tick, &geometry_state));
            require(network_example::solve_legged_locomotion_pose(
                *geometry_skeleton, geometry_bind_pose, geometry_rig, root,
                50.0f, tick, ground_plane, &geometry_state));
        }
        require(geometry_state.pose_valid);

        // One entry per declared collider, in the declared order.
        require(geometry_state.solved_collider_poses.size() ==
                geometry_rig.collider_count);

        const glm::quat root_rotation = glm::angleAxis(
            geometry_state.root_yaw_radians, glm::vec3{0.0f, 1.0f, 0.0f});
        for (std::uint32_t index = 0u; index < geometry_rig.leg_count; ++index) {
            const network_example::SolvedColliderPose& pose =
                geometry_state.solved_collider_poses[index];

            // The GEO_ bone sits exactly on its foot, so composing the
            // collider onto the root transform must land on the foot position
            // the solve computed independently. This is the assertion that
            // pins the seating offset: the pose is seated vertically inside the
            // solve, and a collider frame that forgot that offset would be off
            // by a whole leg length while still looking plausible.
            const glm::vec3 world =
                root + root_rotation * pose.local_position;
            const glm::vec3& foot = geometry_state.legs[index].solved_foot_world;
            require(near(world.x, foot.x));
            require(near(world.y, foot.y));
            require(near(world.z, foot.z));

            // The bone's rest scale is 1.5 x 19 x 1.5, and it must be in the
            // half extents rather than in the rotation. An unnormalised cast of
            // that model matrix is nowhere near a unit quaternion.
            require(near(glm::length(pose.local_rotation), 1.0f));
            const glm::vec3 half_extents =
                network_example::locomotion_collider_half_extents(
                    geometry_bind_pose, geometry_rig.colliders[index]);
            require(near(half_extents.x, kGeoScaleX * 0.5f));
            require(near(half_extents.y, kGeoScaleY * 0.5f));
            require(near(half_extents.z, kGeoScaleX * 0.5f));
        }

        // A collider naming a bone the skeleton does not have publishes no
        // limbs at all, rather than the ones before it in the list. A partial
        // rig would put some segments in the world and leave the rest wherever
        // the previous tick left them.
        KernelSkeletonBindingDefinition broken_geometry = geometry_rig;
        broken_geometry.colliders[1].bone_index = broken_geometry.bone_count;
        require(network_example::advance_locomotion_state(
            broken_geometry, forward_input, 90.0f, tick, &geometry_state));
        require(network_example::solve_legged_locomotion_pose(
            *geometry_skeleton, geometry_bind_pose, broken_geometry, root,
            50.0f, tick, ground_plane, &geometry_state));
        require(geometry_state.solved_collider_poses.empty());

        // Arguments rejected at the door leave the whole state untouched, which
        // is this function's existing contract -- it is a refused call, not a
        // failed solve, and pose_valid is likewise left alone.
        KernelSkeletonBindingDefinition refused = geometry_rig;
        refused.body_bone_index = refused.bone_count;
        require(!network_example::solve_legged_locomotion_pose(
            *geometry_skeleton, geometry_bind_pose, refused, root, 50.0f, tick,
            ground_plane, &geometry_state));

        // A rig that declares none publishes none, which is what keeps per-bone
        // collision opt-in all the way down to the solve.
        KernelSkeletonBindingDefinition bare = geometry_rig;
        bare.collider_count = 0u;
        network_example::LocomotionState bare_state;
        require(network_example::initialize_locomotion_state(
            bare, 0.0f, &bare_state));
        require(network_example::advance_locomotion_state(
            bare, forward_input, 90.0f, tick, &bare_state));
        require(network_example::solve_legged_locomotion_pose(
            *geometry_skeleton, geometry_bind_pose, bare, root, 50.0f, tick,
            ground_plane, &bare_state));
        require(bare_state.solved_collider_poses.empty());
    }
    return 0;
}
