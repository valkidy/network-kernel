licenses(["notice"])

cc_library(
    name = "yaml-cpp",
    srcs = glob([
        "src/**/*.cpp",
    ]),
    hdrs = glob([
        "include/**/*.h",
        "src/**/*.h",
    ]),
    includes = [
        "include",
        "src",
    ],
    defines = ["YAML_CPP_STATIC_DEFINE"],
    visibility = ["//visibility:public"],
)
