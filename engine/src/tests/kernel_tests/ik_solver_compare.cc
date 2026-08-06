// IK solver comparison harness.
//
// Verifies whether ozz's IKTwoBoneJob (what the kernel uses) produces knee/foot
// placements close to the analytic solver in LocomotionTest's TwoBoneIK.cs.
// Both solvers are run on the SAME 3-joint chain, target and pole, over a sweep
// of targets, and the resulting knee/foot world positions are compared.
//
// The ozz path mirrors legged_locomotion.cc exactly: build a real skeleton,
// LocalToModel -> IKTwoBoneJob -> multiply corrections into locals -> LocalToModel.
// The C# path is a direct port of TwoBoneIK.Solve operating on the same geometry.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/span.h"

namespace {

glm::vec3 model_position(const ozz::math::Float4x4& matrix) {
    float values[4];
    ozz::math::StorePtrU(matrix.cols[3], values);
    return glm::vec3{values[0], values[1], values[2]};
}

void multiply_soa_rotation(
    std::uint32_t joint,
    const ozz::math::SimdQuaternion& correction,
    std::vector<ozz::math::SoaTransform>* transforms) {
    ozz::math::SoaTransform& transform = (*transforms)[joint / 4u];
    ozz::math::SimdQuaternion rotations[4];
    ozz::math::Transpose4x4(&transform.rotation.x, &rotations->xyzw);
    rotations[joint & 3u] = rotations[joint & 3u] * correction;
    ozz::math::Transpose4x4(&rotations->xyzw, &transform.rotation.x);
}

// ---- ozz path: mirrors legged_locomotion.cc solve loop for one leg. --------
struct SolveResult {
    glm::vec3 knee{0.0f};
    glm::vec3 foot{0.0f};
};

SolveResult solve_ozz(
    const ozz::animation::Skeleton& skeleton,
    const glm::vec3& target,
    const glm::vec3& pole_dir) {
    std::vector<ozz::math::SoaTransform> locals(
        skeleton.joint_rest_poses().begin(),
        skeleton.joint_rest_poses().end());
    std::vector<ozz::math::Float4x4> models(skeleton.num_joints());
    ozz::animation::LocalToModelJob l2m;
    l2m.skeleton = &skeleton;
    l2m.input = ozz::make_span(locals);
    l2m.output = ozz::make_span(models);
    l2m.Run();

    ozz::animation::IKTwoBoneJob ik;
    ik.target = ozz::math::simd_float4::Load(target.x, target.y, target.z, 0.0f);
    ik.pole_vector =
        ozz::math::simd_float4::Load(pole_dir.x, pole_dir.y, pole_dir.z, 0.0f);
    ik.mid_axis = ozz::math::simd_float4::z_axis();
    ik.start_joint = &models[0];  // hip
    ik.mid_joint = &models[1];    // knee
    ik.end_joint = &models[2];    // foot
    ozz::math::SimdQuaternion hip_c;
    ozz::math::SimdQuaternion knee_c;
    ik.start_joint_correction = &hip_c;
    ik.mid_joint_correction = &knee_c;
    ik.Run();

    multiply_soa_rotation(0u, hip_c, &locals);
    multiply_soa_rotation(1u, knee_c, &locals);
    l2m.Run();

    return SolveResult{model_position(models[1]), model_position(models[2])};
}

// ---- C# path: direct port of TwoBoneIK.Solve, computing positions. ---------
float triangle_angle(float opposite, float a, float b) {
    float cos = (a * a + b * b - opposite * opposite) / (2.0f * a * b);
    return std::acos(std::clamp(cos, -1.0f, 1.0f));
}

glm::vec3 project_on_plane(const glm::vec3& v, const glm::vec3& n) {
    return v - n * glm::dot(v, n);
}

glm::quat from_to(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 f = glm::normalize(from);
    const glm::vec3 t = glm::normalize(to);
    const float d = glm::dot(f, t);
    if (d > 0.99999f) return glm::quat{1, 0, 0, 0};
    if (d < -0.99999f) {
        glm::vec3 axis = glm::cross(glm::vec3{1, 0, 0}, f);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3{0, 1, 0}, f);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::normalize(glm::cross(f, t));
    const float angle = std::acos(std::clamp(d, -1.0f, 1.0f));
    return glm::angleAxis(angle, axis);
}

SolveResult solve_csharp(
    glm::vec3 a, glm::vec3 b, glm::vec3 c,  // hip, knee, foot (world)
    const glm::vec3& target_in,
    const glm::vec3& hint) {
    const float upper_len = glm::length(b - a);
    const float lower_len = glm::length(c - b);
    const float max_reach = (upper_len + lower_len) * 0.999f;
    glm::vec3 to_target = target_in - a;
    float target_dist = std::clamp(
        glm::length(to_target),
        std::abs(upper_len - lower_len) + 1e-4f, max_reach);
    glm::vec3 target = a + glm::normalize(to_target) * target_dist;

    // 1. Bend knee to desired interior angle.
    float current_knee = std::acos(std::clamp(
        glm::dot(glm::normalize(a - b), glm::normalize(c - b)), -1.0f, 1.0f));
    float desired_knee = triangle_angle(target_dist, upper_len, lower_len);
    glm::vec3 bend_axis = glm::cross(c - b, a - b);
    if (glm::dot(bend_axis, bend_axis) < 1e-8f) {
        bend_axis = glm::cross(c - b, hint - b);
        if (glm::dot(bend_axis, bend_axis) < 1e-8f)
            bend_axis = glm::cross(c - b, glm::vec3{0, 1, 0});
    }
    bend_axis = glm::normalize(bend_axis);
    float bend_delta = current_knee - desired_knee;  // radians
    glm::quat r1 = glm::angleAxis(bend_delta, bend_axis);
    c = b + r1 * (c - b);

    // 2. Aim whole limb so tip lands on target.
    glm::quat aim = from_to(c - a, target - a);
    b = a + aim * (b - a);
    c = a + aim * (c - a);

    // 3. Pole swing.
    glm::vec3 aim_dir = glm::normalize(target - a);
    glm::vec3 knee_dir = project_on_plane(b - a, aim_dir);
    glm::vec3 hint_dir = project_on_plane(hint - a, aim_dir);
    if (glm::dot(knee_dir, knee_dir) > 1e-8f &&
        glm::dot(hint_dir, hint_dir) > 1e-8f) {
        glm::quat pole = from_to(knee_dir, hint_dir);
        b = a + pole * (b - a);
        c = a + pole * (c - a);
    }
    return SolveResult{b, c};
}

}  // namespace

int main() {
    // Build a 3-joint chain: hip -> knee -> foot, with a natural forward bend so
    // the bind pose is non-degenerate. Identity rotations => model positions are
    // cumulative local translations.
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    auto& hip = raw.roots[0];
    hip.name = "hip";
    hip.transform.translation = ozz::math::Float3(0.0f, 0.0f, 0.0f);
    hip.transform.rotation = ozz::math::Quaternion::identity();
    hip.transform.scale = ozz::math::Float3(1.0f, 1.0f, 1.0f);
    hip.children.resize(1);
    auto& knee = hip.children[0];
    knee.name = "knee";
    knee.transform.translation = ozz::math::Float3(0.3f, -1.0f, 0.0f);
    knee.transform.rotation = ozz::math::Quaternion::identity();
    knee.transform.scale = ozz::math::Float3(1.0f, 1.0f, 1.0f);
    knee.children.resize(1);
    auto& foot = knee.children[0];
    foot.name = "foot";
    foot.transform.translation = ozz::math::Float3(-0.3f, -1.0f, 0.0f);
    foot.transform.rotation = ozz::math::Quaternion::identity();
    foot.transform.scale = ozz::math::Float3(1.0f, 1.0f, 1.0f);

    ozz::animation::offline::SkeletonBuilder builder;
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton = builder(raw);
    if (!skeleton) {
        std::fprintf(stderr, "skeleton build failed\n");
        return 1;
    }

    const glm::vec3 hip_p{0.0f, 0.0f, 0.0f};
    const glm::vec3 knee_p{0.3f, -1.0f, 0.0f};
    const glm::vec3 foot_p{0.0f, -2.0f, 0.0f};
    const float leg_len = glm::length(knee_p - hip_p) + glm::length(foot_p - knee_p);
    const glm::vec3 pole_dir{1.0f, 0.0f, 0.0f};  // knee bends toward +X
    const float pole_distance = 0.6f;

    // Sweep a set of reachable targets.
    const glm::vec3 targets[] = {
        {0.0f, -1.8f, 0.0f},   // straight down, near full reach
        {0.5f, -1.6f, 0.2f},   // forward + side
        {0.4f, -1.2f, -0.3f},  // bent up, behind
        {-0.3f, -1.5f, 0.4f},  // opposite side
        {0.6f, -1.7f, 0.0f},   // forward, near reach
        {0.0f, -1.0f, 0.5f},   // strongly bent
    };

    std::printf("leg_len=%.4f  pole_dir=(%.1f,%.1f,%.1f)\n",
                leg_len, pole_dir.x, pole_dir.y, pole_dir.z);
    std::printf("%-26s | %-10s | %-24s | %-24s | %-8s\n",
                "target", "foot_err", "ozz_knee", "cs_knee", "knee_d");
    double max_foot_err = 0.0;
    double max_knee_diff = 0.0;
    for (const glm::vec3& t : targets) {
        const SolveResult oz = solve_ozz(*skeleton, t, pole_dir);
        const glm::vec3 mid = (hip_p + t) * 0.5f;
        const glm::vec3 hint = mid + glm::normalize(pole_dir) * pole_distance;
        const SolveResult cs = solve_csharp(hip_p, knee_p, foot_p, t, hint);

        const double foot_err = glm::length(oz.foot - cs.foot);
        const double knee_diff = glm::length(oz.knee - cs.knee);
        max_foot_err = std::max(max_foot_err, foot_err);
        max_knee_diff = std::max(max_knee_diff, knee_diff);
        std::printf(
            "(%5.2f,%5.2f,%5.2f)     | %8.4f  | "
            "(%5.2f,%5.2f,%5.2f)   | (%5.2f,%5.2f,%5.2f)   | %6.4f\n",
            t.x, t.y, t.z, foot_err, oz.knee.x, oz.knee.y, oz.knee.z,
            cs.knee.x, cs.knee.y, cs.knee.z, knee_diff);
    }
    std::printf(
        "\nmax_foot_err=%.4f (%.1f%% leg)  max_knee_diff=%.4f (%.1f%% leg)\n",
        max_foot_err, 100.0 * max_foot_err / leg_len, max_knee_diff,
        100.0 * max_knee_diff / leg_len);
    return 0;
}
