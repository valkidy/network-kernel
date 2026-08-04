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

// Tick-stamped ring of solved poses, so presentation can evaluate the pose as a
// continuous function of render time -- the same way the root transform is
// evaluated -- instead of snapping to whatever the newest simulated tick left
// behind.
//
// This exists because the two halves of a rendered skeleton are produced on
// different schedules: the root is interpolated to a render time that sits
// `snapshot_interval_ticks * 2` behind the simulation, while the solve runs at
// tick rate. Sampling both at one instant is what keeps a planted foot planted:
// the bone locals encode the foot relative to the root of the tick that solved
// them, so composing them onto any other root translates the whole leg by the
// difference. Interpolating them together makes that difference cancel.
struct SkeletonPoseSample {
    std::uint32_t tick = 0;
    std::uint64_t time_us = 0;
    std::vector<KernelBoneLocalTransform> local_transforms;
};

struct SkeletonPoseHistory {
    std::vector<SkeletonPoseSample> samples;
    std::size_t next_index = 0;
    std::size_t size = 0;
};

// Overwrites the oldest slot once `capacity` samples are held. A tick that is
// recorded twice (or out of order, which a rollback could produce) replaces the
// newest slot rather than growing a non-monotonic ring.
void record_skeleton_pose_sample(
    std::uint32_t tick,
    std::uint64_t time_us,
    std::span<const KernelBoneLocalTransform> local_transforms,
    std::size_t capacity,
    SkeletonPoseHistory* history);

// Samples the history at `time_us`, writing the interpolated bone locals and
// the tick the sample was taken from. Clamps to the oldest/newest held sample
// rather than extrapolating, mirroring how snapshot interpolation clamps to the
// ends of its buffer. Returns false when the history holds nothing, leaving the
// outputs untouched so the caller can fall back to the live pose.
bool sample_skeleton_pose_history(
    const SkeletonPoseHistory& history,
    std::uint64_t time_us,
    std::vector<KernelBoneLocalTransform>* out_local_transforms,
    std::uint32_t* out_tick);

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
