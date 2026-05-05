licenses(["notice"])

package(default_visibility = ["//visibility:public"])

config_setting(
    name = "windows",
    values = {"cpu": "x64_windows"},
)

SPDLOG_HEADERS = glob([
    "include/spdlog/**/*.h",
])

cc_library(
    name = "spdlog",
    srcs = [
        "src/async.cpp",
        "src/cfg.cpp",
        "src/color_sinks.cpp",
        "src/file_sinks.cpp",
        "src/spdlog.cpp",
        "src/stdout_sinks.cpp",
    ],
    hdrs = SPDLOG_HEADERS,
    defines = [
        "SPDLOG_COMPILED_LIB",
        "SPDLOG_FMT_EXTERNAL",
    ],
    includes = ["include"],
    linkopts = select({
        ":windows": [],
        "//conditions:default": ["-pthread"],
    }),
    deps = ["@fmt//:fmt"],
)

cc_library(
    name = "spdlog_header_only",
    hdrs = SPDLOG_HEADERS,
    defines = ["SPDLOG_FMT_EXTERNAL"],
    includes = ["include"],
    linkopts = select({
        ":windows": [],
        "//conditions:default": ["-pthread"],
    }),
    deps = ["@fmt//:fmt-header-only"],
)
