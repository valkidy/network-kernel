#include <array>
#include <cassert>
#include <cmath>
#include <vector>

#include "kernel/src/skeleton_presentation.h"

namespace {

KernelBoneLocalTransform transform(float x) {
    return KernelBoneLocalTransform{
        KernelVec3{x, 0.0f, 0.0f},
        KernelQuat{0.0f, 0.0f, 0.0f, 1.0f},
        KernelVec3{1.0f, 1.0f, 1.0f},
    };
}

bool near_equal(float actual, float expected, float epsilon = 0.0001f) {
    return std::abs(actual - expected) <= epsilon;
}

void record(
    std::uint32_t tick,
    std::uint64_t time_us,
    const std::vector<KernelBoneLocalTransform>& transforms,
    std::size_t capacity,
    network_example::SkeletonPoseHistory* history) {
    network_example::record_skeleton_pose_sample(
        tick,
        time_us,
        transforms,
        capacity,
        history);
}

void test_pose_history_bounds() {
    network_example::SkeletonPoseHistory history;
    std::vector<KernelBoneLocalTransform> sampled;
    std::uint32_t sampled_tick = 0u;

    // Nothing recorded: the caller keeps its own fallback pose.
    assert(!network_example::sample_skeleton_pose_history(
        history, 500u, &sampled, &sampled_tick));

    record(1u, 100u, {transform(1.0f)}, 4u, &history);
    assert(network_example::sample_skeleton_pose_history(
        history, 999u, &sampled, &sampled_tick));
    assert(sampled_tick == 1u);
    assert(near_equal(sampled[0].local_position.x, 1.0f));

    record(2u, 200u, {transform(2.0f)}, 4u, &history);
    record(3u, 300u, {transform(3.0f)}, 4u, &history);

    // Clamps to the held ends instead of extrapolating, matching how snapshot
    // interpolation clamps to its buffer.
    assert(network_example::sample_skeleton_pose_history(
        history, 50u, &sampled, &sampled_tick));
    assert(sampled_tick == 1u && near_equal(sampled[0].local_position.x, 1.0f));
    assert(network_example::sample_skeleton_pose_history(
        history, 900u, &sampled, &sampled_tick));
    assert(sampled_tick == 3u && near_equal(sampled[0].local_position.x, 3.0f));

    // Midpoint of the second interval.
    assert(network_example::sample_skeleton_pose_history(
        history, 250u, &sampled, &sampled_tick));
    assert(sampled_tick == 3u && near_equal(sampled[0].local_position.x, 2.5f));
}

void test_pose_history_eviction() {
    network_example::SkeletonPoseHistory history;
    for (std::uint32_t tick = 1u; tick <= 6u; ++tick) {
        record(
            tick,
            static_cast<std::uint64_t>(tick) * 100u,
            {transform(static_cast<float>(tick))},
            4u,
            &history);
    }
    assert(history.size == 4u);

    std::vector<KernelBoneLocalTransform> sampled;
    std::uint32_t sampled_tick = 0u;
    // Ticks 1 and 2 were overwritten; the oldest held sample is tick 3.
    assert(network_example::sample_skeleton_pose_history(
        history, 0u, &sampled, &sampled_tick));
    assert(sampled_tick == 3u && near_equal(sampled[0].local_position.x, 3.0f));
    assert(network_example::sample_skeleton_pose_history(
        history, 450u, &sampled, &sampled_tick));
    assert(near_equal(sampled[0].local_position.x, 4.5f));

    // Re-recording the newest tick replaces it rather than advancing the ring,
    // so ordering stays monotonic.
    record(6u, 600u, {transform(60.0f)}, 4u, &history);
    assert(history.size == 4u);
    assert(network_example::sample_skeleton_pose_history(
        history, 600u, &sampled, &sampled_tick));
    assert(sampled_tick == 6u && near_equal(sampled[0].local_position.x, 60.0f));
}

void test_pose_history_rotation_interpolation() {
    network_example::SkeletonPoseHistory history;
    const float half_quarter = 0.70710678f;  // sin/cos of 45 degrees.
    std::vector<KernelBoneLocalTransform> upright = {transform(0.0f)};
    std::vector<KernelBoneLocalTransform> turned = {transform(0.0f)};
    turned[0].local_rotation = KernelQuat{0.0f, half_quarter, 0.0f, half_quarter};
    record(1u, 100u, upright, 8u, &history);
    record(2u, 200u, turned, 8u, &history);

    std::vector<KernelBoneLocalTransform> sampled;
    std::uint32_t sampled_tick = 0u;
    assert(network_example::sample_skeleton_pose_history(
        history, 150u, &sampled, &sampled_tick));
    // Halfway between 0 and 90 degrees about Y is 45 degrees, i.e. a quaternion
    // of (sin 22.5, cos 22.5) -- a slerp, not a component lerp.
    assert(near_equal(sampled[0].local_rotation.y, std::sin(0.39269908f)));
    assert(near_equal(sampled[0].local_rotation.w, std::cos(0.39269908f)));
}

// The property the whole ring exists for: bone locals encode the foot relative
// to the root of the tick that solved them, so a planted foot only stays
// planted if pose and root are evaluated at the same instant. Model one bone
// whose local position is the foot in model space (identity root rotation) and
// check that composing the interpolated pose onto the interpolated root
// reproduces the world foothold exactly.
void test_planted_foot_survives_interpolation() {
    const float foot_world = 12.0f;
    const float root_at_tick1 = 4.0f;
    const float root_at_tick2 = 4.0f + 2.5f / 30.0f;  // 2.5 m/s for one tick.

    network_example::SkeletonPoseHistory history;
    record(1u, 100000u, {transform(foot_world - root_at_tick1)}, 8u, &history);
    record(2u, 133333u, {transform(foot_world - root_at_tick2)}, 8u, &history);

    for (int step = 0; step <= 10; ++step) {
        const float alpha = static_cast<float>(step) / 10.0f;
        const std::uint64_t time_us = 100000u +
            static_cast<std::uint64_t>(alpha * (133333.0f - 100000.0f));
        std::vector<KernelBoneLocalTransform> sampled;
        std::uint32_t sampled_tick = 0u;
        assert(network_example::sample_skeleton_pose_history(
            history, time_us, &sampled, &sampled_tick));
        const float root =
            root_at_tick1 + (root_at_tick2 - root_at_tick1) * alpha;
        assert(near_equal(root + sampled[0].local_position.x, foot_world, 0.001f));
    }
}

}  // namespace

int main() {
    test_pose_history_bounds();
    test_pose_history_eviction();
    test_pose_history_rotation_interpolation();
    test_planted_foot_survives_interpolation();

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
