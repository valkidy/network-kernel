licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "ozz_base",
    srcs = [
        "src/base/containers/string_archive.cc",
        "src/base/encode/group_varint.cc",
        "src/base/io/archive.cc",
        "src/base/io/stream.cc",
        "src/base/log.cc",
        "src/base/maths/box.cc",
        "src/base/maths/math_archive.cc",
        "src/base/maths/simd_math.cc",
        "src/base/maths/simd_math_archive.cc",
        "src/base/maths/soa_math_archive.cc",
        "src/base/memory/allocator.cc",
        "src/base/platform.cc",
    ],
    hdrs = glob([
        "include/ozz/base/**/*.h",
        "include/ozz/base/**/*.inl",
    ]),
    includes = ["include"],
)

cc_library(
    name = "ozz_animation",
    srcs = [
        "src/animation/runtime/animation.cc",
        "src/animation/runtime/animation_utils.cc",
        "src/animation/runtime/blending_job.cc",
        "src/animation/runtime/ik_aim_job.cc",
        "src/animation/runtime/ik_two_bone_job.cc",
        "src/animation/runtime/local_to_model_job.cc",
        "src/animation/runtime/motion_blending_job.cc",
        "src/animation/runtime/sampling_job.cc",
        "src/animation/runtime/skeleton.cc",
        "src/animation/runtime/skeleton_utils.cc",
        "src/animation/runtime/track.cc",
        "src/animation/runtime/track_sampling_job.cc",
        "src/animation/runtime/track_triggering_job.cc",
    ],
    hdrs = glob([
        "include/ozz/animation/runtime/**/*.h",
        "src/animation/runtime/**/*.h",
    ]),
    includes = [
        "include",
        "src",
    ],
    deps = [":ozz_base"],
)

cc_library(
    name = "ozz_animation_offline",
    srcs = [
        "src/animation/offline/additive_animation_builder.cc",
        "src/animation/offline/animation_builder.cc",
        "src/animation/offline/animation_optimizer.cc",
        "src/animation/offline/motion_extractor.cc",
        "src/animation/offline/raw_animation.cc",
        "src/animation/offline/raw_animation_archive.cc",
        "src/animation/offline/raw_animation_utils.cc",
        "src/animation/offline/raw_skeleton.cc",
        "src/animation/offline/raw_skeleton_archive.cc",
        "src/animation/offline/raw_track.cc",
        "src/animation/offline/raw_track_utils.cc",
        "src/animation/offline/skeleton_builder.cc",
        "src/animation/offline/track_builder.cc",
        "src/animation/offline/track_optimizer.cc",
    ],
    hdrs = glob([
        "include/ozz/animation/offline/*.h",
        "src/animation/offline/*.h",
    ]),
    includes = [
        "include",
        "src",
    ],
    visibility = ["//visibility:private"],
    deps = [":ozz_animation"],
)

cc_library(
    name = "ozz_options",
    srcs = ["src/options/options.cc"],
    hdrs = glob(["include/ozz/options/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:private"],
    deps = [":ozz_base"],
)

cc_library(
    name = "ozz_jsoncpp",
    srcs = ["extern/jsoncpp/dist/jsoncpp.cpp"],
    hdrs = glob(["extern/jsoncpp/dist/json/*.h"]),
    includes = ["extern/jsoncpp/dist"],
    visibility = ["//visibility:private"],
)

cc_library(
    name = "ozz_animation_tools",
    srcs = [
        "src/animation/offline/tools/import2ozz.cc",
        "src/animation/offline/tools/import2ozz_anim.cc",
        "src/animation/offline/tools/import2ozz_config.cc",
        "src/animation/offline/tools/import2ozz_skel.cc",
        "src/animation/offline/tools/import2ozz_track.cc",
    ],
    hdrs = glob([
        "include/ozz/animation/offline/tools/*.h",
        "src/animation/offline/tools/*.h",
    ]),
    includes = [
        "include",
        "src",
    ],
    visibility = ["//visibility:private"],
    deps = [
        ":ozz_animation_offline",
        ":ozz_jsoncpp",
        ":ozz_options",
    ],
)

cc_binary(
    name = "gltf2ozz",
    srcs = [
        "src/animation/offline/gltf/extern/json.hpp",
        "src/animation/offline/gltf/extern/tiny_gltf.h",
        "src/animation/offline/gltf/gltf2ozz.cc",
    ],
    includes = ["src/animation/offline/gltf"],
    deps = [":ozz_animation_tools"],
)
