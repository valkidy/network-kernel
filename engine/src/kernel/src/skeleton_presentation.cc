#include "kernel/src/skeleton_presentation.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <tuple>

namespace network_example {
namespace {

bool invalid_buffer(const void* buffer, std::uint32_t capacity) {
    return buffer == nullptr && capacity != 0u;
}

}  // namespace

std::uint32_t copy_skeleton_render_states(
    std::span<const SkeletonPresentationPose> poses,
    std::uint32_t result_flags,
    std::uint64_t requested_render_time_us,
    KernelSkeletonRenderState* out_states,
    std::uint32_t max_states,
    KernelBoneLocalTransform* out_bone_transforms,
    std::uint32_t max_bone_transforms,
    KernelSkeletonRenderStateResult* out_result) {
    if (out_result == nullptr ||
        out_result->struct_size < sizeof(KernelSkeletonRenderStateResult)) {
        return 0u;
    }

    const std::uint32_t result_size = out_result->struct_size;
    *out_result = KernelSkeletonRenderStateResult{};
    out_result->struct_size = result_size;
    out_result->flags = result_flags;
    out_result->requested_render_time_us = requested_render_time_us;

    if (invalid_buffer(out_states, max_states) ||
        invalid_buffer(out_bone_transforms, max_bone_transforms)) {
        out_result->status = KERNEL_SKELETON_RENDER_STATUS_INVALID_ARGUMENT;
        return 0u;
    }

    std::vector<std::size_t> candidates(poses.size());
    std::iota(candidates.begin(), candidates.end(), 0u);
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [&poses](std::size_t lhs, std::size_t rhs) {
            return std::tie(
                       poses[lhs].entity_net_id,
                       poses[lhs].pose_time_us,
                       poses[lhs].pose_tick) <
                std::tie(
                       poses[rhs].entity_net_id,
                       poses[rhs].pose_time_us,
                       poses[rhs].pose_tick);
        });

    std::vector<std::size_t> order;
    for (std::size_t begin = 0u; begin < candidates.size();) {
        std::size_t end = begin + 1u;
        while (end < candidates.size() &&
               poses[candidates[end]].entity_net_id ==
                   poses[candidates[begin]].entity_net_id) {
            ++end;
        }
        constexpr std::size_t kNoSelection =
            std::numeric_limits<std::size_t>::max();
        std::size_t selected = kNoSelection;
        for (std::size_t cursor = end; cursor > begin; --cursor) {
            const std::size_t candidate = candidates[cursor - 1u];
            if ((result_flags & KERNEL_SKELETON_RENDER_RESULT_FLAG_AT_TIME) == 0u ||
                poses[candidate].pose_time_us <= requested_render_time_us) {
                selected = candidate;
                break;
            }
        }
        if (selected != kNoSelection) {
            order.push_back(selected);
        }
        begin = end;
    }

    std::uint64_t required_bones = 0u;
    for (const std::size_t index : order) {
        const SkeletonPresentationPose& pose = poses[index];
        required_bones += pose.local_transforms.size();
        out_result->source_tick = std::max(out_result->source_tick, pose.pose_tick);
        out_result->evaluated_render_time_us =
            std::max(out_result->evaluated_render_time_us, pose.pose_time_us);
    }
    out_result->required_state_count = static_cast<std::uint32_t>(std::min(
        order.size(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_result->required_bone_transform_count =
        static_cast<std::uint32_t>(std::min(
            required_bones,
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));

    std::uint32_t written_states = 0u;
    std::uint32_t written_bones = 0u;
    for (const std::size_t index : order) {
        const SkeletonPresentationPose& pose = poses[index];
        const std::uint32_t bone_count = static_cast<std::uint32_t>(
            pose.local_transforms.size());
        if (written_states >= max_states ||
            bone_count > max_bone_transforms - written_bones) {
            break;
        }
        out_states[written_states] = KernelSkeletonRenderState{
            pose.entity_net_id,
            pose.skeleton_asset_id,
            pose.skeleton_content_hash,
            written_bones,
            bone_count,
            pose.pose_tick,
            pose.pose_flags,
            pose.pose_time_us,
        };
        if (bone_count != 0u) {
            std::memcpy(
                out_bone_transforms + written_bones,
                pose.local_transforms.data(),
                sizeof(KernelBoneLocalTransform) * bone_count);
        }
        ++written_states;
        written_bones += bone_count;
    }

    out_result->written_state_count = written_states;
    out_result->written_bone_transform_count = written_bones;
    out_result->status =
        written_states == out_result->required_state_count
        ? KERNEL_SKELETON_RENDER_STATUS_SUCCESS
        : KERNEL_SKELETON_RENDER_STATUS_INSUFFICIENT_CAPACITY;
    return written_states;
}

}  // namespace network_example
