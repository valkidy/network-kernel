licenses(["notice"])

package(default_visibility = ["//visibility:public"])

# Python tooling shared by transform capture harnesses.
#
# These are plain sources rather than py_binary targets on purpose: numpy and
# matplotlib cannot be installed into the host interpreter (macOS/homebrew
# pythons are externally managed, PEP 668), so the render and analysis steps run
# under the capture venv that capture_preflight.py provisions, not under Bazel's
# autodetected host toolchain. The orchestrator resolves the interpreter first,
# then runs these scripts with it.

exports_files([
    "analyze_capture.py",
    "capture_preflight.py",
    "render_transforms.py",
    "requirements.txt",
    "run_locomotion_capture.sh",
])

filegroup(
    name = "capture_tools",
    srcs = [
        "analyze_capture.py",
        "capture_preflight.py",
        "render_transforms.py",
        "requirements.txt",
    ],
)
