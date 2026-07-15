licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "jolt",
    srcs = glob(
        ["Jolt/**/*.cpp"],
        exclude = ["Jolt/Renderer/**"],
    ),
    hdrs = glob([
        "Jolt/**/*.h",
        "Jolt/**/*.inl",
    ], exclude = ["Jolt/Renderer/**"]),
    includes = ["."],
)
