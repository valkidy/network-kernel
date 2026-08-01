#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/span.h"

namespace {

constexpr std::array<const char*, 4> kExpectedJointNames{
    "root",
    "hip",
    "knee",
    "foot",
};

void write_u32(std::ofstream* output, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu),
    };
    output->write(bytes.data(), bytes.size());
}

bool make_glb(const std::string& output_path) {
    std::string json =
        R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "root", "children": [1]},
    {"name": "hip", "children": [2], "translation": [0.0, 1.0, 0.0]},
    {"name": "knee", "children": [3], "translation": [0.0, -1.0, 0.0]},
    {"name": "foot", "translation": [0.0, -1.0, 0.0]}
  ],
  "skins": [{
    "skeleton": 0,
    "joints": [0, 1, 2, 3],
    "inverseBindMatrices": 0
  }],
  "accessors": [{
    "bufferView": 0,
    "componentType": 5126,
    "count": 4,
    "type": "MAT4"
  }],
  "animations": [],
  "bufferViews": [{"buffer": 0, "byteLength": 256}],
  "buffers": [{"byteLength": 256}],
  "cameras": [],
  "images": [],
  "materials": [],
  "meshes": [],
  "samplers": [],
  "textures": []
})json";
    while (json.size() % 4u != 0u) {
        json.push_back(' ');
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "failed to create GLB fixture: " << output_path << '\n';
        return false;
    }

    constexpr std::uint32_t kGlbMagic = 0x46546c67u;
    constexpr std::uint32_t kGlbVersion = 2u;
    constexpr std::uint32_t kJsonChunkType = 0x4e4f534au;
    constexpr std::uint32_t kInverseBindMatrixBytes = 4u * 16u * sizeof(float);
    constexpr std::uint32_t kBinChunkType = 0x004e4942u;
    const std::uint32_t total_size = 12u + 8u +
        static_cast<std::uint32_t>(json.size()) + 8u + kInverseBindMatrixBytes;
    write_u32(&output, kGlbMagic);
    write_u32(&output, kGlbVersion);
    write_u32(&output, total_size);
    write_u32(&output, static_cast<std::uint32_t>(json.size()));
    write_u32(&output, kJsonChunkType);
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    write_u32(&output, kInverseBindMatrixBytes);
    write_u32(&output, kBinChunkType);
    constexpr std::array<float, 4> kInverseJointTranslations{
        0.0f,
        -1.0f,
        0.0f,
        1.0f,
    };
    for (float inverse_translation_y : kInverseJointTranslations) {
        for (std::uint32_t element = 0; element < 16u; ++element) {
            float value = element == 0u || element == 5u || element == 10u ||
                    element == 15u
                ? 1.0f
                : 0.0f;
            if (element == 13u) {
                value = inverse_translation_y;
            }
            write_u32(&output, std::bit_cast<std::uint32_t>(value));
        }
    }
    return output.good();
}

bool finite_quaternion(const ozz::math::SimdQuaternion& quaternion) {
    float values[4];
    ozz::math::StorePtrU(quaternion.xyzw, values);
    return std::isfinite(values[0]) && std::isfinite(values[1]) &&
        std::isfinite(values[2]) && std::isfinite(values[3]);
}

bool verify_skeleton(const std::string& skeleton_path) {
    ozz::io::File input(skeleton_path.c_str(), "rb");
    if (!input.opened()) {
        std::cerr << "failed to open generated skeleton: " << skeleton_path << '\n';
        return false;
    }

    ozz::io::IArchive archive(&input);
    if (!archive.TestTag<ozz::animation::Skeleton>()) {
        std::cerr << "generated archive does not contain an ozz skeleton\n";
        return false;
    }

    ozz::animation::Skeleton skeleton;
    archive >> skeleton;
    if (skeleton.num_joints() != static_cast<int>(kExpectedJointNames.size())) {
        std::cerr << "unexpected joint count: " << skeleton.num_joints() << '\n';
        return false;
    }
    for (int index = 0; index < skeleton.num_joints(); ++index) {
        if (std::string(skeleton.joint_names()[index]) !=
            kExpectedJointNames[static_cast<std::size_t>(index)]) {
            std::cerr << "unexpected joint name at index " << index << '\n';
            return false;
        }
    }

    ozz::vector<ozz::math::SoaTransform> local_transforms(
        skeleton.joint_rest_poses().begin(),
        skeleton.joint_rest_poses().end());
    ozz::vector<ozz::math::Float4x4> model_transforms(skeleton.num_joints());
    ozz::animation::LocalToModelJob local_to_model;
    local_to_model.skeleton = &skeleton;
    local_to_model.input = ozz::make_span(local_transforms);
    local_to_model.output = ozz::make_span(model_transforms);
    if (!local_to_model.Run()) {
        std::cerr << "LocalToModelJob failed\n";
        return false;
    }

    ozz::math::SimdQuaternion start_correction;
    ozz::math::SimdQuaternion mid_correction;
    bool reached = false;
    ozz::animation::IKTwoBoneJob ik;
    ik.target = ozz::math::simd_float4::Load(0.5f, -0.5f, 0.0f, 0.0f);
    ik.pole_vector = ozz::math::simd_float4::z_axis();
    ik.mid_axis = ozz::math::simd_float4::z_axis();
    ik.start_joint = &model_transforms[1];
    ik.mid_joint = &model_transforms[2];
    ik.end_joint = &model_transforms[3];
    ik.start_joint_correction = &start_correction;
    ik.mid_joint_correction = &mid_correction;
    ik.reached = &reached;
    if (!ik.Run() || !reached || !finite_quaternion(start_correction) ||
        !finite_quaternion(mid_correction)) {
        std::cerr << "IKTwoBoneJob failed to reach its target or produced "
                     "non-finite output\n";
        return false;
    }

    std::cout << "ozz dependency smoke passed: joints=" << skeleton.num_joints()
              << " reached=" << (reached ? "true" : "false") << '\n';
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ozz_dependency_smoke_test "
                     "--make-glb=<path>|--verify-skeleton=<path>\n";
        return 2;
    }

    const std::string argument(argv[1]);
    constexpr const char* kMakeGlbPrefix = "--make-glb=";
    constexpr const char* kVerifySkeletonPrefix = "--verify-skeleton=";
    if (argument.starts_with(kMakeGlbPrefix)) {
        return make_glb(
                   argument.substr(
                       std::char_traits<char>::length(kMakeGlbPrefix)))
            ? 0
            : 1;
    }
    if (argument.starts_with(kVerifySkeletonPrefix)) {
        return verify_skeleton(
                   argument.substr(
                       std::char_traits<char>::length(kVerifySkeletonPrefix)))
            ? 0
            : 1;
    }

    std::cerr << "unknown argument: " << argument << '\n';
    return 2;
}
