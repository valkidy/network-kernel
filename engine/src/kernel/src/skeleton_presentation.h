#ifndef KERNEL_SRC_SKELETON_PRESENTATION_H_
#define KERNEL_SRC_SKELETON_PRESENTATION_H_

#include <cstdint>
#include <span>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "ozz/animation/runtime/skeleton.h"

namespace network_example {

struct SkeletonPresentationPose {
    std::uint32_t entity_net_id = 0;
    std::uint32_t skeleton_asset_id = 0;
    std::uint64_t skeleton_content_hash = 0;
    std::uint32_t pose_tick = 0;
    std::uint32_t pose_flags = 0;
    std::uint64_t pose_time_us = 0;
    std::vector<KernelBoneLocalTransform> local_transforms;
};

struct RuntimeSkeletonAsset {
    std::uint32_t skeleton_asset_id = 0;
    std::uint64_t skeleton_content_hash = 0;
    ozz::animation::Skeleton skeleton;
    std::vector<KernelBoneLocalTransform> bind_pose;
};

bool load_runtime_skeleton_asset(
    const KernelSkeletonAssetDefinition& definition,
    RuntimeSkeletonAsset* out_asset);

std::uint32_t copy_skeleton_render_states(
    std::span<const SkeletonPresentationPose> poses,
    std::uint32_t result_flags,
    std::uint64_t requested_render_time_us,
    KernelSkeletonRenderState* out_states,
    std::uint32_t max_states,
    KernelBoneLocalTransform* out_bone_transforms,
    std::uint32_t max_bone_transforms,
    KernelSkeletonRenderStateResult* out_result);

}  // namespace network_example

#endif  // KERNEL_SRC_SKELETON_PRESENTATION_H_
