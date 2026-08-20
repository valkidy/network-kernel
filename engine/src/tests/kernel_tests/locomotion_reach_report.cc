// Reports, for a rig's bind pose, how much of each leg's reach the ground under
// its own rest stance already costs.
//
// The leg solve places a foot on the terrain the foothold ray found and then
// clamps the IK target to the limb's own length. If the bind pose parks a foot
// ABOVE the plane the character controller stands the body on, the solve has to
// stretch the limb by that height before it has spent a single centimetre on
// stride or terrain relief -- and if that alone exceeds the reach, every leg is
// clamped on flat ground, at rest, forever. No gait parameter can pay it back.
//
// Usage: locomotion_reach_report <skeleton.ozz>
//
// The leg bone names are the monster's; the report is a diagnostic for that rig
// rather than a general tool.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"

namespace {

glm::vec3 model_position(const ozz::math::Float4x4& matrix) {
    float values[4];
    ozz::math::StorePtrU(matrix.cols[3], values);
    return glm::vec3{values[0], values[1], values[2]};
}

int joint_index(const ozz::animation::Skeleton& skeleton, const char* name) {
    for (int index = 0; index < skeleton.num_joints(); ++index) {
        if (std::string(skeleton.joint_names()[index]) == name) {
            return index;
        }
    }
    std::fprintf(stderr, "missing joint %s\n", name);
    std::abort();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: locomotion_reach_report <skeleton.ozz>\n");
        return 2;
    }

    ozz::io::File file(argv[1], "rb");
    if (!file.opened()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    ozz::io::IArchive archive(&file);
    if (!archive.TestTag<ozz::animation::Skeleton>()) {
        std::fprintf(stderr, "%s is not an ozz skeleton\n", argv[1]);
        return 2;
    }
    ozz::animation::Skeleton skeleton;
    archive >> skeleton;

    // Identity first: the last SoA block has lanes past num_joints that nothing
    // below fills, and a zero quaternion trips ozz's normalization assert.
    ozz::vector<ozz::math::SoaTransform> locals(
        static_cast<std::size_t>(skeleton.num_soa_joints()),
        ozz::math::SoaTransform::identity());
    for (int index = 0; index < skeleton.num_joints(); ++index) {
        const ozz::math::Transform rest =
            ozz::animation::GetJointLocalRestPose(skeleton, index);
        ozz::math::SoaTransform& soa =
            locals[static_cast<std::size_t>(index / 4)];
        const int lane = index % 4;
        reinterpret_cast<float*>(&soa.translation.x)[lane] = rest.translation.x;
        reinterpret_cast<float*>(&soa.translation.y)[lane] = rest.translation.y;
        reinterpret_cast<float*>(&soa.translation.z)[lane] = rest.translation.z;
        reinterpret_cast<float*>(&soa.rotation.x)[lane] = rest.rotation.x;
        reinterpret_cast<float*>(&soa.rotation.y)[lane] = rest.rotation.y;
        reinterpret_cast<float*>(&soa.rotation.z)[lane] = rest.rotation.z;
        reinterpret_cast<float*>(&soa.rotation.w)[lane] = rest.rotation.w;
        reinterpret_cast<float*>(&soa.scale.x)[lane] = rest.scale.x;
        reinterpret_cast<float*>(&soa.scale.y)[lane] = rest.scale.y;
        reinterpret_cast<float*>(&soa.scale.z)[lane] = rest.scale.z;
    }

    ozz::vector<ozz::math::Float4x4> models(
        static_cast<std::size_t>(skeleton.num_joints()));
    ozz::animation::LocalToModelJob job;
    job.skeleton = &skeleton;
    job.input = ozz::make_span(locals);
    job.output = ozz::make_span(models);
    if (!job.Run()) {
        std::fprintf(stderr, "local-to-model failed\n");
        return 2;
    }

    // quadruped_actor.yaml: max_reach_ratio 0.99 on every leg.
    constexpr float kMaxReachRatio = 0.999f;
    const std::array<const char*, 4> legs{
        "FrontLeft", "FrontRight", "RearLeft", "RearRight"};

    struct Leg {
        const char* name;
        glm::vec3 hip;
        glm::vec3 foot;
        float bones;
    };
    std::vector<Leg> measured;
    // solve_legged_locomotion_pose seats the rig by its LOWEST bind foot, so
    // that foot rests exactly on the plane the character controller stands the
    // body on. Every reach below is measured against that same plane.
    float ground_plane_y = 0.0f;
    bool have_ground_plane = false;
    for (const char* leg : legs) {
        const std::string prefix = std::string("JNT_Leg") + leg;
        Leg entry{};
        entry.name = leg;
        entry.hip = model_position(models[static_cast<std::size_t>(
            joint_index(skeleton, (prefix + "_Hip").c_str()))]);
        const glm::vec3 knee =
            model_position(models[static_cast<std::size_t>(
                joint_index(skeleton, (prefix + "_Knee").c_str()))]);
        entry.foot = model_position(models[static_cast<std::size_t>(
            joint_index(skeleton, (prefix + "_Foot").c_str()))]);
        entry.bones = glm::length(knee - entry.hip) +
            glm::length(entry.foot - knee);
        ground_plane_y = have_ground_plane
            ? std::min(ground_plane_y, entry.foot.y)
            : entry.foot.y;
        have_ground_plane = true;
        measured.push_back(entry);
    }

    std::printf("seated ground plane: model y = %.2f\n\n", ground_plane_y);
    std::printf(
        "%-11s %7s %8s %8s %7s %8s %8s %7s %7s\n",
        "leg", "bones", "hip_up", "hip_out", "flat", "flat%", "slack",
        "max_dip", "max_drift");
    for (const Leg& leg : measured) {
        const glm::vec3 stance{leg.foot.x, ground_plane_y, leg.foot.z};
        const float up = leg.hip.y - ground_plane_y;
        const float out = glm::length(
            glm::vec3{leg.hip.x - stance.x, 0.0f, leg.hip.z - stance.z});
        const float flat = glm::length(stance - leg.hip);
        const float reach = leg.bones * kMaxReachRatio;
        // The two ways a foothold spends the slack, each on its own:
        //
        //  max_dip   how far BELOW the body's own ground plane the terrain under
        //            this foot may sit before the IK clamps. The foothold ray is
        //            vertical, so relief moves the foot straight down and the
        //            limb pays for all of it. (Terrain ABOVE the plane shortens
        //            the limb instead and never clamps.)
        //  max_drift how far the planted foot may slide horizontally, straight
        //            away from the hip, before the IK clamps -- the budget the
        //            gait's step_threshold is drawn against.
        const float dip_reach = reach * reach - out * out;
        const float max_dip =
            dip_reach > 0.0f ? std::sqrt(dip_reach) - up : 0.0f;
        const float drift_reach = reach * reach - up * up;
        const float max_drift =
            drift_reach > 0.0f ? std::sqrt(drift_reach) - out : 0.0f;
        std::printf(
            "%-11s %7.2f %8.2f %8.2f %7.2f %7.1f%% %7.2f %7.2f %9.2f\n",
            leg.name,
            leg.bones,
            up,
            out,
            flat,
            100.0f * flat / leg.bones,
            reach - flat,
            max_dip,
            max_drift);
    }

    // How much reach each leg gets back if the body is seated lower than the
    // bind pose (a crouch: same stance width, bent knees), and if the stance is
    // pulled in toward the hip (same body height, feet tucked under). Slack is
    // the budget the gait and the terrain relief have to share -- today it is
    // the "slack" column above.
    std::printf("\nslack (m) if the body is seated LOWER than bind:\n  %-11s",
                "drop");
    const std::array<float, 6> drops{0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
    for (const float drop : drops) {
        std::printf(" %6.1f", drop);
    }
    std::printf("\n");
    for (const Leg& leg : measured) {
        std::printf("  %-11s", leg.name);
        for (const float drop : drops) {
            const float up = leg.hip.y - ground_plane_y - drop;
            const float out = glm::length(glm::vec3{
                leg.hip.x - leg.foot.x, 0.0f, leg.hip.z - leg.foot.z});
            std::printf(
                " %6.2f",
                leg.bones * kMaxReachRatio -
                    std::sqrt(up * up + out * out));
        }
        std::printf("\n");
    }

    std::printf("\nslack (m) if the stance is pulled IN toward the hip:\n"
                "  %-11s", "scale");
    const std::array<float, 6> scales{1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f};
    for (const float scale : scales) {
        std::printf(" %6.2f", scale);
    }
    std::printf("\n");
    for (const Leg& leg : measured) {
        std::printf("  %-11s", leg.name);
        for (const float scale : scales) {
            const float up = leg.hip.y - ground_plane_y;
            const float out = scale * glm::length(glm::vec3{
                leg.hip.x - leg.foot.x, 0.0f, leg.hip.z - leg.foot.z});
            std::printf(
                " %6.2f",
                leg.bones * kMaxReachRatio -
                    std::sqrt(up * up + out * out));
        }
        std::printf("\n");
    }
    return 0;
}
