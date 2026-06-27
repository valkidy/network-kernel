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
