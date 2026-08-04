#include "kernel/src/legged_locomotion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numbers>
#include <utility>

#include <glm/gtc/quaternion.hpp>

#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/span.h"

namespace network_example {
namespace {

bool valid_definition(const KernelSkeletonBindingDefinition& definition) {
    if (definition.struct_size < sizeof(KernelSkeletonBindingDefinition) ||
        definition.leg_count == 0u ||
        definition.leg_count > KERNEL_MAX_SKELETON_LEGS ||
        definition.processing_order_count != definition.leg_count ||
        !std::isfinite(definition.step_threshold_meters) ||
        definition.step_threshold_meters <= 0.0f ||
        definition.step_duration_ticks == 0u ||
        definition.max_swinging_legs == 0u ||
        definition.max_swinging_legs > definition.leg_count ||
        !std::isfinite(definition.input_deadzone) ||
        definition.input_deadzone < 0.0f ||
        definition.input_deadzone >= 1.0f ||
        !std::isfinite(definition.body_follow_speed) ||
        definition.body_follow_speed < 0.0f ||
        !std::isfinite(definition.slope_alignment) ||
        definition.slope_alignment < 0.0f ||
        definition.slope_alignment > 1.0f ||
        definition.foothold_query_type != KernelFootholdQueryType_Raycast ||
        !std::isfinite(definition.foothold_query_start_height_meters) ||
        definition.foothold_query_start_height_meters < 0.0f ||
        !std::isfinite(definition.foothold_query_distance_meters) ||
        definition.foothold_query_distance_meters <= 0.0f ||
        definition.foothold_candidate_count == 0u ||
        definition.foothold_candidate_count >
            KERNEL_MAX_FOOTHOLD_CANDIDATES) {
        return false;
    }
    for (std::uint32_t candidate = 0u;
         candidate < definition.foothold_candidate_count;
         ++candidate) {
        const KernelVec2& offset =
            definition.foothold_candidate_offsets[candidate];
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
            return false;
        }
    }
    std::array<bool, KERNEL_MAX_SKELETON_LEGS> ordered{};
    for (std::uint32_t order = 0u; order < definition.leg_count; ++order) {
        const std::uint32_t leg_index = definition.processing_order[order];
        if (leg_index >= definition.leg_count || ordered[leg_index]) {
            return false;
        }
        ordered[leg_index] = true;
    }
    for (std::uint32_t leg = 0u; leg < definition.leg_count; ++leg) {
        if (definition.legs[leg].gait_group >= definition.leg_count) {
            return false;
        }
    }
    return true;
}

float shortest_angle_delta(float from, float to) {
    return std::remainder(
        to - from,
        2.0f * std::numbers::pi_v<float>);
}

bool finite_vec3(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

glm::vec3 model_position(const ozz::math::Float4x4& matrix) {
    float values[4];
    ozz::math::StorePtrU(matrix.cols[3], values);
    return glm::vec3{values[0], values[1], values[2]};
}

void multiply_soa_rotation(
    std::uint32_t joint,
    const ozz::math::SimdQuaternion& correction,
    std::span<ozz::math::SoaTransform> transforms) {
    ozz::math::SoaTransform& transform = transforms[joint / 4u];
    ozz::math::SimdQuaternion rotations[4];
    ozz::math::Transpose4x4(&transform.rotation.x, &rotations->xyzw);
    rotations[joint & 3u] = rotations[joint & 3u] * correction;
    ozz::math::Transpose4x4(&rotations->xyzw, &transform.rotation.x);
}

void write_local_rotations(
    std::span<const ozz::math::SoaTransform> transforms,
    std::uint32_t joint_count,
    std::vector<KernelBoneLocalTransform>* pose) {
    for (std::uint32_t block = 0u; block < transforms.size(); ++block) {
        ozz::math::SimdFloat4 rotations[4];
        ozz::math::Transpose4x4(
            &transforms[block].rotation.x,
            rotations);
        for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
            const std::uint32_t joint = block * 4u + lane;
            if (joint >= joint_count) {
                return;
            }
            float values[4];
            ozz::math::StorePtrU(rotations[lane], values);
            (*pose)[joint].local_rotation = KernelQuat{
                values[0], values[1], values[2], values[3]};
        }
    }
}

bool finite_pose(std::span<const KernelBoneLocalTransform> pose) {
    for (const KernelBoneLocalTransform& transform : pose) {
        const float values[] = {
            transform.local_position.x,
            transform.local_position.y,
            transform.local_position.z,
            transform.local_rotation.x,
            transform.local_rotation.y,
            transform.local_rotation.z,
            transform.local_rotation.w,
            transform.local_scale.x,
            transform.local_scale.y,
            transform.local_scale.z,
        };
        if (!std::all_of(std::begin(values), std::end(values), [](float value) {
                return std::isfinite(value);
            })) {
            return false;
        }
    }
    return true;
}

// Raycast the terrain under a leg's home stance, trying each authored candidate
// offset (rotated into the body heading) until one lands on a walkable slope.
// Mirrors ProceduralLeg.SampleGround with extra fan-out. Reach is intentionally
// NOT filtered here (LocomotionTest handles reach purely via IK clamping); a
// foothold the leg cannot fully reach still steers the planted foot, and the IK
// stage clamps the target so the leg stretches toward it without popping.
bool query_foothold(
    const KernelSkeletonBindingDefinition& definition,
    const glm::vec3& nominal_world,
    const glm::quat& root_rotation,
    float minimum_ground_normal_y,
    const LocomotionGroundingQuery& grounding_query,
    LegLocomotionState* leg) {
    leg->ground_hit_valid = false;
    leg->grounding_candidate_index = UINT32_MAX;
    leg->supporting_entity_net_id = 0u;
    leg->supporting_collider_id = 0u;
    if (!grounding_query) {
        return false;
    }
    for (std::uint32_t candidate = 0u;
         candidate < definition.foothold_candidate_count;
         ++candidate) {
        const KernelVec2& authored =
            definition.foothold_candidate_offsets[candidate];
        const glm::vec3 offset = root_rotation *
            glm::vec3{authored.x, 0.0f, authored.y};
        const glm::vec3 origin = nominal_world + offset +
            glm::vec3{0.0f,
                      definition.foothold_query_start_height_meters,
                      0.0f};
        LocomotionGroundingHit hit;
        if (!grounding_query(
                origin,
                definition.foothold_query_distance_meters,
                &hit) ||
            !finite_vec3(hit.position) || !finite_vec3(hit.normal)) {
            continue;
        }
        const float normal_length = glm::length(hit.normal);
        if (normal_length <= 0.000001f) {
            continue;
        }
        hit.normal /= normal_length;
        if (hit.normal.y < minimum_ground_normal_y) {
            continue;
        }
        leg->ground_hit_valid = true;
        leg->grounding_candidate_index = candidate;
        leg->ground_hit_position = hit.position;
        leg->ground_hit_normal = hit.normal;
        leg->supporting_entity_net_id = hit.supporting_entity_net_id;
        leg->supporting_collider_id = hit.supporting_collider_id;
        return true;
    }
    return false;
}

}  // namespace

bool validate_locomotion_definition(
    const KernelSkeletonBindingDefinition& definition) {
    return valid_definition(definition);
}

bool initialize_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    float initial_root_yaw_radians,
    LocomotionState* out_state) {
    if (out_state == nullptr || !valid_definition(definition) ||
        !std::isfinite(initial_root_yaw_radians)) {
        return false;
    }
    LocomotionState state;
    state.root_yaw_radians = initial_root_yaw_radians;
    state.legs.reserve(definition.leg_count);
    state.last_processing_order.reserve(definition.leg_count);
    for (std::uint32_t index = 0u; index < definition.leg_count; ++index) {
        const KernelSkeletonLegDefinition& leg = definition.legs[index];
        LegLocomotionState leg_state;
        leg_state.hip_bone_index = leg.hip_bone_index;
        leg_state.knee_bone_index = leg.knee_bone_index;
        leg_state.foot_bone_index = leg.foot_bone_index;
        leg_state.gait_group = leg.gait_group;
        state.legs.push_back(leg_state);
    }
    for (std::uint32_t order = 0u; order < definition.leg_count; ++order) {
        state.last_processing_order.push_back(
            definition.processing_order[order]);
    }
    *out_state = std::move(state);
    return true;
}

bool advance_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    const KernelVec2& move_input,
    float max_yaw_degrees_per_second,
    float fixed_delta_seconds,
    LocomotionState* state) {
    if (state == nullptr || !valid_definition(definition) ||
        state->legs.size() != definition.leg_count ||
        !std::isfinite(move_input.x) || !std::isfinite(move_input.y) ||
        !std::isfinite(max_yaw_degrees_per_second) ||
        max_yaw_degrees_per_second <= 0.0f ||
        !std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0f) {
        return false;
    }

    // Input movement is authored in the world-plane as (x, 0, y); heading is the
    // yaw that faces that direction. Only meaningful input rotates the actor.
    const float move_magnitude_squared =
        move_input.x * move_input.x + move_input.y * move_input.y;
    const bool move_active = move_magnitude_squared >=
        definition.input_deadzone * definition.input_deadzone;
    bool yaw_changed = false;
    if (move_active) {
        const float target_yaw = std::atan2(move_input.x, move_input.y);
        const float max_yaw_step = max_yaw_degrees_per_second *
            std::numbers::pi_v<float> / 180.0f * fixed_delta_seconds;
        const float yaw_step = std::clamp(
            shortest_angle_delta(state->root_yaw_radians, target_yaw),
            -max_yaw_step,
            max_yaw_step);
        yaw_changed = std::abs(yaw_step) > 1e-6f;
        state->root_yaw_radians += yaw_step;
        state->root_yaw_radians = std::remainder(
            state->root_yaw_radians,
            2.0f * std::numbers::pi_v<float>);
    }
    // A leg only takes a new step when the actor intends to move or turn
    // (LocomotionTest: legs change only when movement.sqrMagnitude > 0 or the
    // heading changes). Idle actors keep their feet planted.
    state->locomotion_active = move_active || yaw_changed;

    state->last_processing_order.clear();
    for (std::uint32_t order = 0u; order < definition.leg_count; ++order) {
        const std::uint32_t leg_index = definition.processing_order[order];
        LegLocomotionState& leg = state->legs[leg_index];
        leg.entered_swing = false;
        leg.entered_support = false;
        state->last_processing_order.push_back(leg_index);
    }
    return true;
}

bool solve_legged_locomotion_pose(
    const ozz::animation::Skeleton& skeleton,
    std::span<const KernelBoneLocalTransform> bind_pose,
    const KernelSkeletonBindingDefinition& definition,
    const glm::vec3& root_position,
    float max_slope_degrees,
    float fixed_delta_seconds,
    const LocomotionGroundingQuery& grounding_query,
    LocomotionState* state) {
    if (state == nullptr || !valid_definition(definition) ||
        skeleton.num_joints() != static_cast<int>(bind_pose.size()) ||
        definition.bone_count != bind_pose.size() ||
        state->legs.size() != definition.leg_count ||
        !finite_vec3(root_position) ||
        !std::isfinite(max_slope_degrees) || max_slope_degrees <= 0.0f ||
        max_slope_degrees >= 90.0f ||
        !std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0f) {
        return false;
    }

    state->pose_valid = false;
    state->body_follow_valid = false;
    state->local_pose.assign(bind_pose.begin(), bind_pose.end());

    std::vector<ozz::math::SoaTransform> locals(
        skeleton.joint_rest_poses().begin(),
        skeleton.joint_rest_poses().end());
    std::vector<ozz::math::Float4x4> models(skeleton.num_joints());
    ozz::animation::LocalToModelJob local_to_model;
    local_to_model.skeleton = &skeleton;
    local_to_model.input = ozz::make_span(locals);
    local_to_model.output = ozz::make_span(models);
    if (!local_to_model.Run()) {
        return false;
    }

    const glm::quat root_rotation = glm::angleAxis(
        state->root_yaw_radians,
        glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::quat inverse_root_rotation = glm::inverse(root_rotation);

    // Vertically seat the bind pose so the lowest foot rests on the ground plane
    // when the root is at terrain height (presentation alignment; body follow,
    // when enabled, adjusts the root height on top of this).
    float bind_foot_height = model_position(
        models[definition.legs[0].foot_bone_index]).y;
    for (std::uint32_t leg_index = 1u;
         leg_index < definition.leg_count;
         ++leg_index) {
        bind_foot_height = std::min(
            bind_foot_height,
            model_position(
                models[definition.legs[leg_index].foot_bone_index]).y);
    }
    const glm::vec3 pose_root_position = root_position -
        glm::vec3{0.0f, bind_foot_height, 0.0f};
    state->local_pose[definition.root_bone_index].local_position.y -=
        bind_foot_height;
    const float minimum_ground_normal_y =
        std::cos(max_slope_degrees * std::numbers::pi_v<float> / 180.0f);

    std::array<glm::vec3, KERNEL_MAX_SKELETON_LEGS> hip_models{};
    std::array<glm::vec3, KERNEL_MAX_SKELETON_LEGS> bind_foot_models{};
    std::array<float, KERNEL_MAX_SKELETON_LEGS> maximum_reaches{};
    std::array<glm::vec3, KERNEL_MAX_SKELETON_LEGS> home_world{};
    std::array<bool, KERNEL_MAX_SKELETON_LEGS> home_grounded{};

    // Pass 1: sample each leg's home foothold and seed uninitialized feet.
    for (const std::uint32_t leg_index : state->last_processing_order) {
        const KernelSkeletonLegDefinition& definition_leg =
            definition.legs[leg_index];
        LegLocomotionState& leg = state->legs[leg_index];

        const glm::vec3 hip_model =
            model_position(models[definition_leg.hip_bone_index]);
        const glm::vec3 knee_model =
            model_position(models[definition_leg.knee_bone_index]);
        const glm::vec3 foot_model =
            model_position(models[definition_leg.foot_bone_index]);
        hip_models[leg_index] = hip_model;
        bind_foot_models[leg_index] = foot_model;
        maximum_reaches[leg_index] =
            (glm::length(knee_model - hip_model) +
             glm::length(foot_model - knee_model)) *
            definition_leg.max_reach_ratio;

        // Rest stance under the current body pose (ProceduralLeg.HomeWorld).
        const glm::vec3 nominal_world =
            pose_root_position + root_rotation * foot_model;
        home_grounded[leg_index] = query_foothold(
            definition,
            nominal_world,
            root_rotation,
            minimum_ground_normal_y,
            grounding_query,
            &leg);
        home_world[leg_index] =
            home_grounded[leg_index] ? leg.ground_hit_position : nominal_world;

        // A foot commits to a world position only once it has a real foothold;
        // until then the limb keeps its bind pose (ProceduralLeg.Initialize
        // plants the foot on the ground before the leg is driven).
        if (!leg.foot_initialized && home_grounded[leg_index]) {
            leg.foot_target_world = home_world[leg_index];
            leg.gait_state = LegGaitState::kSupport;
            leg.swing_tick = 0u;
            leg.foot_initialized = true;
        }
    }

    // Pass 2: gait coordination. A leg may only begin a step when the actor is
    // moving, its home has drifted past the threshold, and no leg of a different
    // gait group is currently swinging (diagonal/alternating trot). Ongoing
    // swings always finish regardless of input.
    std::array<bool, KERNEL_MAX_SKELETON_LEGS> group_swinging{};
    std::uint32_t swinging_group_count = 0u;
    std::uint32_t swinging_leg_count = 0u;
    for (const LegLocomotionState& leg : state->legs) {
        if (leg.gait_state != LegGaitState::kSwing) {
            continue;
        }
        ++swinging_leg_count;
        if (!group_swinging[leg.gait_group]) {
            group_swinging[leg.gait_group] = true;
            ++swinging_group_count;
        }
    }
    if (state->locomotion_active) {
        for (const std::uint32_t leg_index : state->last_processing_order) {
            LegLocomotionState& leg = state->legs[leg_index];
            if (!leg.foot_initialized ||
                leg.gait_state != LegGaitState::kSupport ||
                !home_grounded[leg_index] ||
                swinging_leg_count >= definition.max_swinging_legs) {
                continue;
            }
            // Blocked if another group is mid-swing.
            const bool other_group_swinging = swinging_group_count > 0u &&
                !(swinging_group_count == 1u &&
                  group_swinging[leg.gait_group]);
            if (other_group_swinging) {
                continue;
            }
            const float error =
                glm::length(leg.foot_target_world - home_world[leg_index]);
            if (error <= definition.step_threshold_meters) {
                continue;
            }
            leg.gait_state = LegGaitState::kSwing;
            leg.entered_swing = true;
            leg.swing_tick = 0u;
            leg.swing_start_world = leg.foot_target_world;
            leg.landing_target_world = home_world[leg_index];
            ++swinging_leg_count;
            if (!group_swinging[leg.gait_group]) {
                group_swinging[leg.gait_group] = true;
                ++swinging_group_count;
            }
        }
    }

    // Pass 3: animate feet (swing arc / hold) and solve two-bone IK per leg.
    for (const std::uint32_t leg_index : state->last_processing_order) {
        const KernelSkeletonLegDefinition& definition_leg =
            definition.legs[leg_index];
        LegLocomotionState& leg = state->legs[leg_index];
        // Uninitialized legs (no foothold yet) keep their bind pose.
        if (!leg.foot_initialized) {
            leg.solved_foot_world = pose_root_position +
                root_rotation * bind_foot_models[leg_index];
            continue;
        }
        const glm::vec3 hip_model = hip_models[leg_index];
        const float maximum_reach = maximum_reaches[leg_index];

        if (leg.gait_state == LegGaitState::kSwing) {
            // Re-target the landing spot in case the body kept moving mid-step.
            if (home_grounded[leg_index]) {
                leg.landing_target_world = home_world[leg_index];
            }
            const float swing_phase = definition.step_duration_ticks <= 1u
                ? 1.0f
                : std::min(
                    1.0f,
                    static_cast<float>(leg.swing_tick) /
                        static_cast<float>(
                            definition.step_duration_ticks - 1u));
            const glm::vec3 flat = glm::mix(
                leg.swing_start_world,
                leg.landing_target_world,
                swing_phase);
            const float lift = definition_leg.step_height_meters *
                std::sin(swing_phase * std::numbers::pi_v<float>);
            leg.foot_target_world = flat + glm::vec3{0.0f, lift, 0.0f};
            if (swing_phase >= 1.0f) {
                leg.gait_state = LegGaitState::kSupport;
                leg.entered_support = true;
                leg.swing_tick = 0u;
                leg.foot_target_world = leg.landing_target_world;
            } else {
                ++leg.swing_tick;
            }
        }
        // Support legs simply hold foot_target_world in place.

        glm::vec3 target_model = inverse_root_rotation *
            (leg.foot_target_world - pose_root_position);
        const glm::vec3 hip_to_target = target_model - hip_model;
        const float target_distance = glm::length(hip_to_target);
        leg.ik_reach_clamped = target_distance > maximum_reach &&
            maximum_reach > 0.000001f;
        if (leg.ik_reach_clamped) {
            target_model = hip_model +
                hip_to_target / target_distance * maximum_reach;
        }

        const glm::vec3 pole = glm::normalize(glm::vec3{
            definition_leg.pole_local.x,
            definition_leg.pole_local.y,
            definition_leg.pole_local.z,
        });
        ozz::animation::IKTwoBoneJob ik;
        ik.target = ozz::math::simd_float4::Load(
            target_model.x, target_model.y, target_model.z, 0.0f);
        ik.pole_vector = ozz::math::simd_float4::Load(
            pole.x, pole.y, pole.z, 0.0f);
        ik.mid_axis = ozz::math::simd_float4::z_axis();
        ik.start_joint = &models[definition_leg.hip_bone_index];
        ik.mid_joint = &models[definition_leg.knee_bone_index];
        ik.end_joint = &models[definition_leg.foot_bone_index];
        ozz::math::SimdQuaternion hip_correction;
        ozz::math::SimdQuaternion knee_correction;
        ik.start_joint_correction = &hip_correction;
        ik.mid_joint_correction = &knee_correction;
        if (!ik.Run()) {
            state->local_pose.assign(bind_pose.begin(), bind_pose.end());
            return false;
        }
        multiply_soa_rotation(
            definition_leg.hip_bone_index,
            hip_correction,
            locals);
        multiply_soa_rotation(
            definition_leg.knee_bone_index,
            knee_correction,
            locals);
        local_to_model.from =
            static_cast<int>(definition_leg.hip_bone_index);
        local_to_model.to = ozz::animation::Skeleton::kMaxJoints;
        if (!local_to_model.Run()) {
            state->local_pose.assign(bind_pose.begin(), bind_pose.end());
            return false;
        }
        leg.solved_foot_world = pose_root_position + root_rotation *
            model_position(models[definition_leg.foot_bone_index]);
    }

    // Body grounding follow (optional): expose the terrain-fit body height and
    // tilt so the kernel can blend the transform toward them. Height sits above
    // the average foothold by the rig's rest stance; tilt aims at the average
    // ground normal. Left disabled (physics owns the body) when speed <= 0.
    if (definition.body_follow_speed > 0.0f) {
        float foot_y_sum = 0.0f;
        float bind_foot_y_sum = 0.0f;
        glm::vec3 normal_sum{0.0f};
        std::uint32_t contributing = 0u;
        for (const std::uint32_t leg_index : state->last_processing_order) {
            const LegLocomotionState& leg = state->legs[leg_index];
            if (!leg.foot_initialized) {
                continue;
            }
            foot_y_sum += leg.foot_target_world.y;
            bind_foot_y_sum += bind_foot_models[leg_index].y;
            normal_sum += home_grounded[leg_index]
                ? leg.ground_hit_normal
                : glm::vec3{0.0f, 1.0f, 0.0f};
            ++contributing;
        }
        if (contributing > 0u) {
            const float count = static_cast<float>(contributing);
            const float rest_body_height = -(bind_foot_y_sum / count);
            state->body_follow_target_height =
                foot_y_sum / count + rest_body_height;
            state->body_follow_ground_normal =
                glm::dot(normal_sum, normal_sum) > 0.000001f
                    ? glm::normalize(normal_sum)
                    : glm::vec3{0.0f, 1.0f, 0.0f};
            state->body_follow_valid =
                std::isfinite(state->body_follow_target_height) &&
                finite_vec3(state->body_follow_ground_normal);
        }
    }

    write_local_rotations(
        locals,
        static_cast<std::uint32_t>(skeleton.num_joints()),
        &state->local_pose);
    state->pose_valid = finite_pose(state->local_pose);
    if (!state->pose_valid) {
        state->local_pose.assign(bind_pose.begin(), bind_pose.end());
    }
    return state->pose_valid;
}

}  // namespace network_example
