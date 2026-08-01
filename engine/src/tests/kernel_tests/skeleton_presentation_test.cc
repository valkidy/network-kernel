#include <array>
#include <cassert>

#include "kernel/src/skeleton_presentation.h"

namespace {

KernelBoneLocalTransform transform(float x) {
    return KernelBoneLocalTransform{
        KernelVec3{x, 0.0f, 0.0f},
        KernelQuat{0.0f, 0.0f, 0.0f, 1.0f},
        KernelVec3{1.0f, 1.0f, 1.0f},
    };
}

}  // namespace

int main() {
    std::array<network_example::SkeletonPresentationPose, 3> poses{};
    poses[0].entity_net_id = 42u;
    poses[0].skeleton_asset_id = 9u;
    poses[0].skeleton_content_hash = 123u;
    poses[0].pose_tick = 8u;
    poses[0].pose_time_us = 800u;
    poses[0].local_transforms = {transform(2.0f), transform(3.0f)};
    poses[1].entity_net_id = 7u;
    poses[1].skeleton_asset_id = 9u;
    poses[1].skeleton_content_hash = 123u;
    poses[1].pose_tick = 7u;
    poses[1].pose_flags = KERNEL_SKELETON_POSE_FLAG_BIND_POSE;
    poses[1].pose_time_us = 700u;
    poses[1].local_transforms = {transform(1.0f)};
    poses[2].entity_net_id = 42u;
    poses[2].skeleton_asset_id = 9u;
    poses[2].skeleton_content_hash = 123u;
    poses[2].pose_tick = 6u;
    poses[2].pose_time_us = 600u;
    poses[2].local_transforms = {transform(4.0f), transform(5.0f)};

    KernelSkeletonRenderStateResult result{};
    result.struct_size = sizeof(result);
    assert(network_example::copy_skeleton_render_states(
               poses, 0u, 900u, nullptr, 0u, nullptr, 0u, &result) == 0u);
    assert(result.status == KERNEL_SKELETON_RENDER_STATUS_INSUFFICIENT_CAPACITY);
    assert(result.required_state_count == 2u);
    assert(result.required_bone_transform_count == 3u);
    assert(result.written_state_count == 0u);
    assert(result.source_tick == 8u);
    assert(result.requested_render_time_us == 900u);
    assert(result.evaluated_render_time_us == 800u);

    std::array<KernelSkeletonRenderState, 2> states{};
    std::array<KernelBoneLocalTransform, 3> bones{};
    result.struct_size = sizeof(result);
    assert(network_example::copy_skeleton_render_states(
               poses,
               0u,
               900u,
               states.data(),
               states.size(),
               bones.data(),
               2u,
               &result) == 1u);
    assert(result.status == KERNEL_SKELETON_RENDER_STATUS_INSUFFICIENT_CAPACITY);
    assert(result.written_bone_transform_count == 1u);
    assert(states[0].entity_net_id == 7u);
    assert(states[0].first_bone_transform == 0u);
    assert(states[0].bone_count == 1u);
    assert(bones[0].local_position.x == 1.0f);

    result.struct_size = sizeof(result);
    assert(network_example::copy_skeleton_render_states(
               poses,
               KERNEL_SKELETON_RENDER_RESULT_FLAG_AT_TIME,
               750u,
               states.data(),
               states.size(),
               bones.data(),
               bones.size(),
               &result) == 2u);
    assert(states[1].entity_net_id == 42u);
    assert(states[1].pose_time_us == 600u);
    assert(bones[1].local_position.x == 4.0f);

    result.struct_size = sizeof(result);
    assert(network_example::copy_skeleton_render_states(
               poses,
               KERNEL_SKELETON_RENDER_RESULT_FLAG_AT_TIME,
               900u,
               states.data(),
               states.size(),
               bones.data(),
               bones.size(),
               &result) == 2u);
    assert(result.status == KERNEL_SKELETON_RENDER_STATUS_SUCCESS);
    assert(result.written_bone_transform_count == 3u);
    assert(states[0].entity_net_id == 7u);
    assert(states[1].entity_net_id == 42u);
    assert(states[1].first_bone_transform == 1u);
    assert(bones[1].local_position.x == 2.0f);

    result.struct_size = sizeof(result);
    assert(network_example::copy_skeleton_render_states(
               poses,
               0u,
               0u,
               nullptr,
               1u,
               nullptr,
               0u,
               &result) == 0u);
    assert(result.status == KERNEL_SKELETON_RENDER_STATUS_INVALID_ARGUMENT);
}
