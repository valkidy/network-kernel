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
        definition.gait_cycle_ticks == 0u ||
        definition.gait_swing_ticks == 0u ||
        definition.gait_swing_ticks >= definition.gait_cycle_ticks ||
        definition.max_swinging_legs == 0u ||
        definition.max_swinging_legs > definition.leg_count ||
        !std::isfinite(definition.input_deadzone) ||
        definition.input_deadzone < 0.0f ||
        definition.input_deadzone >= 1.0f ||
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
    std::uint32_t authored_max_swinging = 0u;
    for (std::uint32_t origin = 0u; origin < definition.leg_count; ++origin) {
        const std::uint32_t origin_offset =
            definition.legs[origin].phase_offset_ticks %
            definition.gait_cycle_ticks;
        const std::uint32_t gait_phase =
            (definition.gait_cycle_ticks - origin_offset) %
            definition.gait_cycle_ticks;
        std::uint32_t swinging = 0u;
        for (std::uint32_t leg = 0u; leg < definition.leg_count; ++leg) {
            const std::uint32_t leg_phase = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(gait_phase) +
                 definition.legs[leg].phase_offset_ticks %
                     definition.gait_cycle_ticks) %
                definition.gait_cycle_ticks);
            swinging += leg_phase < definition.gait_swing_ticks ? 1u : 0u;
        }
        authored_max_swinging = std::max(authored_max_swinging, swinging);
    }
    if (authored_max_swinging > definition.max_swinging_legs) {
        return false;
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
    std::uint32_t simulation_tick,
    LocomotionState* out_state) {
    if (out_state == nullptr || !valid_definition(definition) ||
        !std::isfinite(initial_root_yaw_radians)) {
        return false;
    }
    LocomotionState state;
    state.root_yaw_radians = initial_root_yaw_radians;
    state.gait_start_tick = simulation_tick;
    state.legs.reserve(definition.leg_count);
    state.last_processing_order.reserve(definition.leg_count);
    for (std::uint32_t index = 0u; index < definition.leg_count; ++index) {
        const KernelSkeletonLegDefinition& leg = definition.legs[index];
        state.legs.push_back(LegLocomotionState{
            leg.hip_bone_index,
            leg.knee_bone_index,
            leg.foot_bone_index,
            0u,
            LegGaitState::kSupport,
        });
    }
    *out_state = std::move(state);
    return true;
}

bool advance_locomotion_state(
    const KernelSkeletonBindingDefinition& definition,
    const KernelVec2& move_input,
    float max_yaw_degrees_per_second,
    float fixed_delta_seconds,
    std::uint32_t simulation_tick,
    LocomotionState* state) {
    if (state == nullptr || !valid_definition(definition) ||
        state->legs.size() != definition.leg_count ||
        !std::isfinite(move_input.x) || !std::isfinite(move_input.y) ||
        !std::isfinite(max_yaw_degrees_per_second) ||
        max_yaw_degrees_per_second <= 0.0f ||
        !std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0f) {
        return false;
    }

    const float move_magnitude_squared =
        move_input.x * move_input.x + move_input.y * move_input.y;
    if (move_magnitude_squared >=
        definition.input_deadzone * definition.input_deadzone) {
        const float target_yaw = std::atan2(move_input.x, move_input.y);
        const float max_yaw_step = max_yaw_degrees_per_second *
            std::numbers::pi_v<float> / 180.0f * fixed_delta_seconds;
        state->root_yaw_radians += std::clamp(
            shortest_angle_delta(state->root_yaw_radians, target_yaw),
            -max_yaw_step,
            max_yaw_step);
        state->root_yaw_radians = std::remainder(
            state->root_yaw_radians,
            2.0f * std::numbers::pi_v<float>);
    }

    state->gait_phase_tick =
        (simulation_tick - state->gait_start_tick) %
        definition.gait_cycle_ticks;
    state->last_processing_order.clear();
    for (std::uint32_t order = 0u; order < definition.leg_count; ++order) {
        const std::uint32_t leg_index = definition.processing_order[order];
        const KernelSkeletonLegDefinition& definition_leg =
            definition.legs[leg_index];
        LegLocomotionState& leg = state->legs[leg_index];
        const LegGaitState previous_gait_state = leg.gait_state;
        leg.phase_tick = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(state->gait_phase_tick) +
             definition_leg.phase_offset_ticks) %
            definition.gait_cycle_ticks);
        leg.gait_state = leg.phase_tick < definition.gait_swing_ticks
            ? LegGaitState::kSwing
            : LegGaitState::kSupport;
        leg.entered_swing = previous_gait_state == LegGaitState::kSupport &&
            leg.gait_state == LegGaitState::kSwing;
        leg.entered_support = previous_gait_state == LegGaitState::kSwing &&
            leg.gait_state == LegGaitState::kSupport;
        state->last_processing_order.push_back(leg_index);
    }
    return true;
}

bool solve_legged_locomotion_pose(
    const ozz::animation::Skeleton& skeleton,
    std::span<const KernelBoneLocalTransform> bind_pose,
    const KernelSkeletonBindingDefinition& definition,
    const glm::vec3& root_position,
    const glm::vec3& root_velocity,
    float max_slope_degrees,
    float fixed_delta_seconds,
    const LocomotionGroundingQuery& grounding_query,
    LocomotionState* state) {
    if (state == nullptr || !valid_definition(definition) ||
        skeleton.num_joints() != static_cast<int>(bind_pose.size()) ||
        definition.bone_count != bind_pose.size() ||
        state->legs.size() != definition.leg_count ||
        !finite_vec3(root_position) || !finite_vec3(root_velocity) ||
        !std::isfinite(max_slope_degrees) || max_slope_degrees <= 0.0f ||
        max_slope_degrees >= 90.0f ||
        !std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0f) {
        return false;
    }

    state->pose_valid = false;
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
    const float minimum_ground_normal_y =
        std::cos(max_slope_degrees * std::numbers::pi_v<float> / 180.0f);

    for (const std::uint32_t leg_index : state->last_processing_order) {
        const KernelSkeletonLegDefinition& definition_leg =
            definition.legs[leg_index];
        LegLocomotionState& leg = state->legs[leg_index];
        const glm::vec3 bind_foot_model =
            model_position(models[definition_leg.foot_bone_index]);
        const std::uint32_t remaining_swing_ticks =
            leg.gait_state == LegGaitState::kSwing
            ? definition.gait_swing_ticks - leg.phase_tick
            : 0u;
        const glm::vec3 predicted_root_position = root_position +
            root_velocity * fixed_delta_seconds *
                static_cast<float>(remaining_swing_ticks);
        const glm::vec3 nominal_world = predicted_root_position +
            root_rotation * bind_foot_model;

        if (leg.gait_state == LegGaitState::kSwing) {
            if (leg.entered_swing || !leg.foot_target_valid) {
                leg.swing_start_world = leg.foot_target_valid
                    ? leg.foot_target_world
                    : root_position + root_rotation * bind_foot_model;
            }
            leg.planted = false;
            if (leg.entered_swing || !leg.landing_target_valid) {
                leg.landing_target_valid = query_foothold(
                    definition,
                    nominal_world,
                    root_rotation,
                    minimum_ground_normal_y,
                    grounding_query,
                    &leg);
            }
            leg.landing_target_world = leg.landing_target_valid
                ? leg.ground_hit_position
                : nominal_world;
            const float swing_phase = definition.gait_swing_ticks <= 1u
                ? 1.0f
                : static_cast<float>(leg.phase_tick) /
                    static_cast<float>(definition.gait_swing_ticks - 1u);
            leg.foot_target_world = glm::mix(
                leg.swing_start_world,
                leg.landing_target_world,
                swing_phase);
            leg.foot_target_world.y +=
                4.0f * definition_leg.step_height_meters * swing_phase *
                (1.0f - swing_phase);
            leg.foot_target_valid = true;
        } else {
            if (leg.entered_support && leg.landing_target_valid) {
                leg.planted_foothold_world = leg.landing_target_world;
                leg.planted = true;
            }
            if (!leg.planted) {
                leg.planted = query_foothold(
                    definition,
                    root_position + root_rotation * bind_foot_model,
                    root_rotation,
                    minimum_ground_normal_y,
                    grounding_query,
                    &leg);
                if (leg.planted) {
                    leg.planted_foothold_world = leg.ground_hit_position;
                }
            }
            leg.foot_target_world = leg.planted
                ? leg.planted_foothold_world
                : root_position + root_rotation * bind_foot_model;
            leg.foot_target_valid = true;
        }

        glm::vec3 target_model = inverse_root_rotation *
            (leg.foot_target_world - root_position);
        const glm::vec3 hip_model =
            model_position(models[definition_leg.hip_bone_index]);
        const glm::vec3 knee_model =
            model_position(models[definition_leg.knee_bone_index]);
        const glm::vec3 foot_model =
            model_position(models[definition_leg.foot_bone_index]);
        const float maximum_reach =
            (glm::length(knee_model - hip_model) +
             glm::length(foot_model - knee_model)) *
            definition_leg.max_reach_ratio;
        glm::vec3 hip_to_target = target_model - hip_model;
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
