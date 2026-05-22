workspace(name = "network-example")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "com_google_absl",
    patch_args = [
        "-p1",
    ],
    patches = [
        "@//third_party:com_google_absl_windows_patch.diff",
    ],
    sha256 = "f841f78243f179326f2a80b719f2887c38fe226d288ecdc46e2aa091e6aa43bc",
    strip_prefix = "abseil-cpp-9687a8ea750bfcddf790372093245a1d041b21a3",
    urls = ["https://github.com/abseil/abseil-cpp/archive//9687a8ea750bfcddf790372093245a1d041b21a3.tar.gz"],
)

http_archive(
    name = "rules_cc",
    patch_args = ["-p1"],
    patches = ["@//third_party:rules_cc.diff"],
    sha256 = "0d3b4f984c4c2e1acfd1378e0148d35caf2ef1d9eb95b688f8e19ce0c41bdf5b",
    strip_prefix = "rules_cc-0.1.4",
    url = "https://github.com/bazelbuild/rules_cc/releases/download/0.1.4/rules_cc-0.1.4.tar.gz",
)

http_archive(
    name = "rules_java",
    sha256 = "c73336802d0b4882e40770666ad055212df4ea62cfa6edf9cb0f9d29828a0934",
    url = "https://github.com/bazelbuild/rules_java/releases/download/5.3.5/rules_java-5.3.5.tar.gz",
)

http_archive(
    name = "rules_pkg",
    sha256 = "8a298e832762eda1830597d64fe7db58178aa84cd5926d76d5b744d6558941c2",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/rules_pkg/releases/download/0.7.0/rules_pkg-0.7.0.tar.gz",
        "https://github.com/bazelbuild/rules_pkg/releases/download/0.7.0/rules_pkg-0.7.0.tar.gz",
    ],
)

new_local_repository(
    name = "zlib",
    build_file_content = """
cc_library(
    name = "zlib",
    hdrs = [
        "usr/include/zconf.h",
        "usr/include/zlib.h",
    ],
    includes = ["usr/include"],
    linkopts = ["-lz"],
    visibility = ["//visibility:public"],
)
""",
    path = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
)

new_local_repository(
    name = "system_openssl",
    build_file_content = """
cc_library(
    name = "openssl",
    hdrs = glob([
        "include/openssl/**/*.h",
    ]),
    includes = ["include"],
    linkopts = [
        "-L/opt/homebrew/opt/openssl@3/lib",
        "-lssl",
        "-lcrypto",
    ],
    visibility = ["//visibility:public"],
)
""",
    path = "/opt/homebrew/opt/openssl@3",
)

http_archive(
    name = "com_google_protobuf",
    patch_args = [
        "-p1",
    ],
    patches = [
        "@//third_party:com_google_protobuf_fixes.diff",
    ],
    sha256 = "f645e6e42745ce922ca5388b1883ca583bafe4366cc74cf35c3c9299005136e2",
    strip_prefix = "protobuf-5.28.3",
    urls = ["https://github.com/protocolbuffers/protobuf/archive/refs/tags/v5.28.3.zip"],
)

# GameNetworkingSockets
http_archive(
    name = "GameNetworkingSockets",
    sha256 = "1dd8b122cf742dadc70da22c73ec446cb0327358312338efc3f862370c1a60a2",
    strip_prefix = "GameNetworkingSockets-1.5.1",
    urls = ["https://github.com/ValveSoftware/GameNetworkingSockets/archive/refs/tags/v1.5.1.tar.gz"],
    build_file = "@//third_party:GameNetworkingSockets.BUILD",
)

# entt
http_archive(
    name = "entt",
    sha256 = "7d7b4037b737992342049ffab14f22fa10243e01664f8c3a0657aa247ac52f71",
    strip_prefix = "entt-3.16.0",
    urls = ["https://github.com/skypjack/entt/archive/refs/tags/v3.16.0.tar.gz"],
    build_file = "@//third_party:entt.BUILD",
)

# Eigen
# org_tensorflow depends on Eigen. If updating tensorflow version,
# make sure to bump Eigen version as well and vice versa.
EIGEN_COMMIT = "4c38131a16803130b66266a912029504f2cf23cd"

EIGEN_SHA256 = "1a432ccbd597ea7b9faa1557b1752328d6adc1a3db8969f6fe793ff704be3bf0"

http_archive(
    name = "eigen",
    build_file = "@//third_party:eigen.BUILD",
    sha256 = EIGEN_SHA256,
    strip_prefix = "eigen-{commit}".format(commit = EIGEN_COMMIT),
    urls = ["https://gitlab.com/libeigen/eigen/-/archive/{commit}/eigen-{commit}.tar.gz".format(commit = EIGEN_COMMIT)],
)

# glm
http_archive(
    name = "glm",
    sha256 = "19edf2e860297efab1c74950e6076bf4dad9de483826bc95e2e0f2c758a43f65",
    strip_prefix = "glm-1.0.2",
    urls = ["https://github.com/g-truc/glm/archive/refs/tags/1.0.2.tar.gz"],
    build_file = "@//third_party:glm.BUILD",
)

# flatbuffers
load("@//third_party/flatbuffers:workspace.bzl", flatbuffers = "repo")

flatbuffers()

# nlohmann
http_archive(
    name = "com_github_nlohmann_json",
    build_file = "@//third_party:nlohmann.BUILD",
    sha256 = "6bea5877b1541d353bd77bdfbdb2696333ae5ed8f9e8cc22df657192218cad91",
    urls = ["https://github.com/nlohmann/json/releases/download/v3.9.1/include.zip"],
)

# spdlog
http_archive(
    name = "spdlog",
    strip_prefix = "spdlog-1.17.0",
    sha256 = "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744",
    urls = ["https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz"],
    build_file = "@//third_party:spdlog.BUILD",
)

# fmt
http_archive(
    name = "fmt",
    strip_prefix = "fmt-12.1.0",
    sha256 = "ea7de4299689e12b6dddd392f9896f08fb0777ac7168897a244a6d6085043fea",
    urls = ["https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz"],
    build_file = "@//third_party:fmt.BUILD",
)

# yaml
http_archive(
    name = "yaml-cpp",
    sha256 = "298593d9c440fd9034b8b193d96318b76d49bc97c6ceadb7b0836edf0b6d7539",
    urls = ["https://github.com/jbeder/yaml-cpp/releases/download/yaml-cpp-0.9.0/yaml-cpp-yaml-cpp-0.9.0.tar.gz"],
    build_file = "@//third_party:yaml-cpp.BUILD",
)
