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

cc_binary(
    name = "ozz_dependency_smoke_driver",
    srcs = ["ozz_dependency_smoke_test.cc"],
    deps = ["@ozz_animation//:ozz_animation"],
)

genrule(
    name = "ozz_smoke_glb",
    outs = ["ozz_smoke.glb"],
    cmd = "$(location :ozz_dependency_smoke_driver) --make-glb=$@",
    tools = [":ozz_dependency_smoke_driver"],
)

genrule(
    name = "ozz_smoke_skeleton",
    srcs = [":ozz_smoke_glb"],
    outs = ["ozz_smoke.ozz"],
    cmd = """
$(location @ozz_animation//:gltf2ozz) \
  --file=$(location :ozz_smoke_glb) \
  --config='{\"skeleton\":{\"filename\":\"$@\",\"import\":{\"enable\":true}},\"animations\":[]}'
""",
    tools = ["@ozz_animation//:gltf2ozz"],
)

cc_test(
    name = "ozz_dependency_smoke_test",
    srcs = ["ozz_dependency_smoke_test.cc"],
    args = ["--verify-skeleton=$(location :ozz_smoke_skeleton)"],
    data = [":ozz_smoke_skeleton"],
    size = "small",
    deps = ["@ozz_animation//:ozz_animation"],
)
