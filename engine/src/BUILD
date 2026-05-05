licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "example_app",
    srcs = ["main.cc"],
    data = [
        "@flatbuffers//:flatc",
        "@flatbuffers//:runtime_java",
        "@flatbuffers//:runtime_py",
    ],
    deps = [
        "@GameNetworkingSockets//:steam_headers",
        "@com_github_nlohmann_json//:json",
        "@eigen//:eigen3",
        "@entt//:entt",
        "@flatbuffers//:runtime_cc",
        "@fmt//:fmt",
        "@glm//:glm",
        "@spdlog//:spdlog",
    ],
)
