#!/usr/bin/env python3
"""Checks the host environment a capture's Python tooling needs.

The capture harness itself is hermetic Bazel C++, but rendering an mp4 needs
host-installed numpy, matplotlib and ffmpeg (there is no pip toolchain in this
WORKSPACE). Run this before anything expensive so a missing package fails in a
second instead of after two full simulation runs, and with an error that says
exactly what to install.

Exits 0 when every check passes, 1 otherwise.
"""
import argparse
import importlib
import os
import shutil
import subprocess
import sys

MIN_PYTHON = (3, 9)

# module import name -> pip package name
REQUIRED_MODULES = {
    "numpy": "numpy",
    "matplotlib": "matplotlib",
}


class Report:
    def __init__(self):
        self.lines = []
        self.failures = []

    def ok(self, check, detail=""):
        self.lines.append(f"PASS  {check}" + (f": {detail}" if detail else ""))

    def fail(self, check, detail, remedy):
        self.lines.append(f"FAIL  {check}: {detail}")
        self.failures.append((check, detail, remedy))

    def text(self):
        out = list(self.lines)
        if self.failures:
            out.append("")
            out.append("Capture preflight failed. Fix the following and re-run:")
            for check, detail, remedy in self.failures:
                out.append(f"  - {check}: {detail}")
                out.append(f"    {remedy}")
        return "\n".join(out) + "\n"


def check_python(report):
    version = sys.version_info
    text = f"{version.major}.{version.minor}.{version.micro} ({sys.executable})"
    if (version.major, version.minor) >= MIN_PYTHON:
        report.ok("python3", text)
    else:
        report.fail(
            "python3",
            f"{text} is older than {MIN_PYTHON[0]}.{MIN_PYTHON[1]}",
            "Install a newer python3 and make sure it is first on PATH.",
        )


def check_modules(report):
    missing = []
    for module_name, package_name in REQUIRED_MODULES.items():
        try:
            module = importlib.import_module(module_name)
        except ImportError as error:
            report.fail(
                f"python module {module_name}",
                str(error),
                f"{sys.executable} -m pip install {package_name}",
            )
            missing.append(package_name)
            continue
        report.ok(
            f"python module {module_name}",
            getattr(module, "__version__", "unknown version"),
        )
    if len(missing) > 1:
        report.failures.append(
            (
                "python modules",
                "several packages are missing",
                f"{sys.executable} -m pip install " + " ".join(missing),
            )
        )


def check_ffmpeg(report):
    path = shutil.which("ffmpeg")
    if path is None:
        report.fail(
            "ffmpeg",
            "not found on PATH",
            "brew install ffmpeg",
        )
        return
    try:
        result = subprocess.run(
            [path, "-version"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        report.fail("ffmpeg", f"{path} is not runnable: {error}", "brew install ffmpeg")
        return
    if result.returncode != 0:
        report.fail(
            "ffmpeg",
            f"{path} -version exited {result.returncode}",
            "brew reinstall ffmpeg",
        )
        return
    first_line = result.stdout.splitlines()[0] if result.stdout else path
    report.ok("ffmpeg", first_line)


def check_matplotlib_writer(report):
    try:
        import matplotlib

        matplotlib.use("Agg")
        from matplotlib.animation import FFMpegWriter
    except ImportError:
        # Already reported by check_modules.
        return
    if FFMpegWriter.isAvailable():
        report.ok("matplotlib FFMpegWriter", "available")
    else:
        report.fail(
            "matplotlib FFMpegWriter",
            "matplotlib cannot find an ffmpeg it can drive",
            "Ensure ffmpeg is on PATH, or set "
            "matplotlib.rcParams['animation.ffmpeg_path'].",
        )


def check_output_dir(report, path):
    if not path:
        return
    try:
        os.makedirs(path, exist_ok=True)
        probe = os.path.join(path, ".preflight_write_probe")
        with open(probe, "w", encoding="utf-8") as handle:
            handle.write("ok")
        os.remove(probe)
    except OSError as error:
        report.fail(
            "output directory",
            f"{path} is not writable: {error}",
            f"Fix the permissions on {path} or pass a different output path.",
        )
        return
    report.ok("output directory", path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        default="",
        help="directory the capture will write to; checked for writability",
    )
    parser.add_argument(
        "--error-log",
        default="",
        help="write the full report here when a check fails",
    )
    args = parser.parse_args()

    report = Report()
    check_python(report)
    check_modules(report)
    check_ffmpeg(report)
    check_matplotlib_writer(report)
    check_output_dir(report, args.output_dir)

    text = report.text()
    sys.stdout.write(text)
    if not report.failures:
        return 0

    if args.error_log:
        try:
            os.makedirs(os.path.dirname(args.error_log) or ".", exist_ok=True)
            with open(args.error_log, "w", encoding="utf-8") as handle:
                handle.write(text)
            sys.stderr.write(f"preflight error log: {args.error_log}\n")
        except OSError as error:
            sys.stderr.write(f"could not write {args.error_log}: {error}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
