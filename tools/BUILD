licenses(["notice"])

package(default_visibility = ["//visibility:public"])

py_binary(
    name = "kernel_rpc_codegen",
    srcs = ["kernel_rpc_codegen.py"],
    main = "kernel_rpc_codegen.py",
    python_version = "PY3",
)

py_test(
    name = "kernel_rpc_codegen_test",
    srcs = [
        "kernel_rpc_codegen.py",
        "kernel_rpc_codegen_test.py",
    ],
    main = "kernel_rpc_codegen_test.py",
    python_version = "PY3",
)

sh_test(
    name = "check_cross_platform_boundaries",
    srcs = ["check_cross_platform_boundaries.sh"],
)

cc_binary(
    name = "static_mesh_bake_smoke",
    srcs = ["static_mesh_bake_smoke.cc"],
    data = ["@recastnavigation//:dungeon_obj"],
    deps = [
        "//engine/src/geometry:geometry",
        "//engine/src/geometry:mesh_bake",
        "@bazel_tools//tools/cpp/runfiles",
    ],
)

cc_binary(
    name = "mesh_asset_converter",
    srcs = ["mesh_asset_converter.cc"],
    deps = [
        "//engine/src/geometry:geometry",
        "//engine/src/geometry:mesh_bake",
    ],
)
