licenses(["notice"])

package(default_visibility = ["//visibility:public"])

# Python tooling shared by transform capture harnesses. These targets run on the
# host interpreter (this WORKSPACE has no pip toolchain), so anything that
# imports numpy/matplotlib must be gated on :capture_preflight first.

exports_files(["run_locomotion_capture.sh"])

py_binary(
    name = "capture_preflight",
    srcs = ["capture_preflight.py"],
    main = "capture_preflight.py",
    python_version = "PY3",
)

py_binary(
    name = "render_transforms",
    srcs = ["render_transforms.py"],
    main = "render_transforms.py",
    python_version = "PY3",
)

py_binary(
    name = "analyze_capture",
    srcs = ["analyze_capture.py"],
    main = "analyze_capture.py",
    python_version = "PY3",
)
