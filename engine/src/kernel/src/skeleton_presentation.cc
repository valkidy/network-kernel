#include "kernel/src/skeleton_presentation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"

namespace network_example {
namespace {

bool invalid_buffer(const void* buffer, std::uint32_t capacity) {
    return buffer == nullptr && capacity != 0u;
}

std::uint64_t fnv1a64(const std::uint8_t* bytes, std::uint32_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint32_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool finite_transform(const KernelBoneLocalTransform& transform) {
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
    return std::all_of(std::begin(values), std::end(values), [](float value) {
        return std::isfinite(value);
    });
}

}  // namespace

bool load_runtime_skeleton_asset(
    const KernelSkeletonAssetDefinition& definition,
    RuntimeSkeletonAsset* out_asset) {
    if (out_asset == nullptr ||
        definition.struct_size < sizeof(KernelSkeletonAssetDefinition) ||
        definition.skeleton_asset_id == 0u ||
        definition.skeleton_content_hash == 0u ||
        definition.runtime_skeleton_data == nullptr ||
        definition.runtime_skeleton_size == 0u ||
        definition.bone_count == 0u || definition.bone_count > 1024u ||
        fnv1a64(
            definition.runtime_skeleton_data,
            definition.runtime_skeleton_size) !=
            definition.skeleton_content_hash) {
        return false;
    }

    ozz::io::MemoryStream stream;
    if (stream.Write(
            definition.runtime_skeleton_data,
            definition.runtime_skeleton_size) !=
            definition.runtime_skeleton_size ||
        stream.Seek(0, ozz::io::Stream::kSet) != 0) {
        return false;
    }
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<ozz::animation::Skeleton>()) {
        return false;
    }

    RuntimeSkeletonAsset asset;
    archive >> asset.skeleton;
    if (asset.skeleton.num_joints() !=
        static_cast<int>(definition.bone_count)) {
        return false;
    }
    std::unordered_set<std::string_view> names;
    asset.bind_pose.reserve(definition.bone_count);
    for (std::uint32_t index = 0; index < definition.bone_count; ++index) {
        if (!names.insert(asset.skeleton.joint_names()[index]).second) {
            return false;
        }
        const ozz::math::Transform rest_pose =
            ozz::animation::GetJointLocalRestPose(
                asset.skeleton,
                static_cast<int>(index));
        const KernelBoneLocalTransform transform{
            KernelVec3{
                rest_pose.translation.x,
                rest_pose.translation.y,
                rest_pose.translation.z,
            },
            KernelQuat{
                rest_pose.rotation.x,
                rest_pose.rotation.y,
                rest_pose.rotation.z,
                rest_pose.rotation.w,
            },
            KernelVec3{
                rest_pose.scale.x,
                rest_pose.scale.y,
                rest_pose.scale.z,
            },
        };
        if (!finite_transform(transform)) {
            return false;
        }
        asset.bind_pose.push_back(transform);
    }
    asset.skeleton_asset_id = definition.skeleton_asset_id;
    asset.skeleton_content_hash = definition.skeleton_content_hash;
    *out_asset = std::move(asset);
    return true;
}

void record_skeleton_pose_sample(
    std::uint32_t tick,
    std::uint64_t time_us,
    std::span<const KernelBoneLocalTransform> local_transforms,
    std::size_t capacity,
    SkeletonPoseHistory* history) {
    if (history == nullptr || capacity == 0u) {
        return;
    }
    if (history->samples.size() != capacity) {
        history->samples.assign(capacity, SkeletonPoseSample{});
        history->next_index = 0u;
        history->size = 0u;
    }
    // Re-recording the newest tick overwrites it instead of advancing, so the
    // ring stays strictly increasing in time and the search below can rely on
    // that ordering.
    std::size_t slot = history->next_index;
    if (history->size != 0u) {
        const std::size_t newest =
            (history->next_index + capacity - 1u) % capacity;
        if (history->samples[newest].tick >= tick) {
            slot = newest;
        }
    }
    SkeletonPoseSample& sample = history->samples[slot];
    sample.tick = tick;
    sample.time_us = time_us;
    sample.local_transforms.assign(
        local_transforms.begin(),
        local_transforms.end());
    if (slot != history->next_index) {
        return;
    }
    history->next_index = (history->next_index + 1u) % capacity;
    history->size = std::min(history->size + 1u, capacity);
}

bool sample_skeleton_pose_history(
    const SkeletonPoseHistory& history,
    std::uint64_t time_us,
    std::vector<KernelBoneLocalTransform>* out_local_transforms,
    std::uint32_t* out_tick) {
    if (out_local_transforms == nullptr || out_tick == nullptr ||
        history.size == 0u || history.samples.empty()) {
        return false;
    }
    const std::size_t capacity = history.samples.size();
    const auto held = [&](std::size_t index) -> const SkeletonPoseSample& {
        const std::size_t oldest =
            (history.next_index + capacity - history.size) % capacity;
        return history.samples[(oldest + index) % capacity];
    };

    const SkeletonPoseSample& oldest = held(0u);
    const SkeletonPoseSample& newest = held(history.size - 1u);
    if (history.size == 1u || time_us <= oldest.time_us) {
        *out_local_transforms = oldest.local_transforms;
        *out_tick = oldest.tick;
        return true;
    }
    if (time_us >= newest.time_us) {
        *out_local_transforms = newest.local_transforms;
        *out_tick = newest.tick;
        return true;
    }

    const SkeletonPoseSample* from = &oldest;
    const SkeletonPoseSample* to = &newest;
    for (std::size_t index = 1u; index < history.size; ++index) {
        const SkeletonPoseSample& candidate = held(index);
        if (candidate.time_us >= time_us) {
            from = &held(index - 1u);
            to = &candidate;
            break;
        }
    }
    if (to->time_us <= from->time_us ||
        from->local_transforms.size() != to->local_transforms.size()) {
        *out_local_transforms = to->local_transforms;
        *out_tick = to->tick;
        return true;
    }

    const float alpha = static_cast<float>(time_us - from->time_us) /
        static_cast<float>(to->time_us - from->time_us);
    out_local_transforms->resize(from->local_transforms.size());
    for (std::size_t bone = 0u; bone < from->local_transforms.size(); ++bone) {
        const KernelBoneLocalTransform& a = from->local_transforms[bone];
        const KernelBoneLocalTransform& b = to->local_transforms[bone];
        const glm::vec3 position = glm::mix(
            glm::vec3{a.local_position.x, a.local_position.y, a.local_position.z},
            glm::vec3{b.local_position.x, b.local_position.y, b.local_position.z},
            alpha);
        const glm::quat rotation = glm::normalize(glm::slerp(
            glm::quat{
                a.local_rotation.w,
                a.local_rotation.x,
                a.local_rotation.y,
                a.local_rotation.z},
            glm::quat{
                b.local_rotation.w,
                b.local_rotation.x,
                b.local_rotation.y,
                b.local_rotation.z},
            alpha));
        const glm::vec3 scale = glm::mix(
            glm::vec3{a.local_scale.x, a.local_scale.y, a.local_scale.z},
            glm::vec3{b.local_scale.x, b.local_scale.y, b.local_scale.z},
            alpha);
        (*out_local_transforms)[bone] = KernelBoneLocalTransform{
            KernelVec3{position.x, position.y, position.z},
            KernelQuat{rotation.x, rotation.y, rotation.z, rotation.w},
            KernelVec3{scale.x, scale.y, scale.z},
        };
    }
    // The pose is reported against the tick it was interpolated toward; the
    // exact instant it was evaluated at travels separately as pose_time_us.
    *out_tick = to->tick;
    return true;
}

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
