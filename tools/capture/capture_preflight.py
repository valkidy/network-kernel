#!/usr/bin/env python3
"""Prepares and checks the Python environment a capture's tooling needs.

Rendering an mp4 needs numpy, matplotlib and ffmpeg, but the host interpreter is
never touched: macOS/homebrew pythons are externally managed (PEP 668), so this
creates a project-local virtualenv (capture/.venv by default), installs
tools/capture/requirements.txt into it, and reports back the interpreter the
capture should run its Python steps with. Subsequent runs reuse the venv and
only re-check that the imports resolve.

Runs on the host interpreter with nothing but the standard library, so it can
diagnose a machine where nothing is installed yet.

Exits 0 when the environment is ready, 1 otherwise. With --write-interpreter the
path of the ready-to-use interpreter is written to that file.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

MIN_PYTHON = (3, 9)

PROBE_IMPORTS = r"""
import importlib, json, sys
report = {}
for name in sys.argv[1:]:
    try:
        module = importlib.import_module(name)
    except Exception as error:
        report[name] = {"ok": False, "detail": str(error)}
    else:
        report[name] = {
            "ok": True,
            "detail": getattr(module, "__version__", "unknown version"),
        }
print(json.dumps(report))
"""

PROBE_WRITER = r"""
import json
try:
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib.animation import FFMpegWriter
except Exception as error:
    print(json.dumps({"ok": False, "detail": str(error)}))
else:
    print(json.dumps({"ok": bool(FFMpegWriter.isAvailable()), "detail": "FFMpegWriter"}))
"""


class Report:
    def __init__(self):
        self.lines = []
        self.failures = []

    def ok(self, check, detail=""):
        self.lines.append(f"PASS  {check}" + (f": {detail}" if detail else ""))

    def fail(self, check, detail, remedy):
        self.lines.append(f"FAIL  {check}: {detail}")
        self.failures.append((check, detail, remedy))

    def note(self, text):
        self.lines.append(f"      {text}")

    def text(self):
        out = list(self.lines)
        if self.failures:
            out.append("")
            out.append("Capture preflight failed. Fix the following and re-run:")
            for check, detail, remedy in self.failures:
                out.append(f"  - {check}: {detail}")
                out.append(f"    {remedy}")
        return "\n".join(out) + "\n"


def read_requirements(path):
    """Returns (requirement lines, import names to probe)."""
    lines = []
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if line and not line.startswith("#"):
                lines.append(line)
    modules = []
    for line in lines:
        name = line
        for separator in ("==", ">=", "<=", "~=", ">", "<", "[", ";"):
            name = name.split(separator)[0]
        modules.append(name.strip().replace("-", "_"))
    return lines, modules


def run_probe(interpreter, source, arguments=()):
    try:
        result = subprocess.run(
            [interpreter, "-c", source, *arguments],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        return None, str(error)
    if result.returncode != 0:
        return None, (result.stderr or result.stdout).strip()
    try:
        return json.loads(result.stdout.strip().splitlines()[-1]), ""
    except (ValueError, IndexError):
        return None, result.stdout.strip()


def check_host_python(report):
    version = sys.version_info
    text = f"{version.major}.{version.minor}.{version.micro} ({sys.executable})"
    if (version.major, version.minor) >= MIN_PYTHON:
        report.ok("host python3", text)
        return True
    report.fail(
        "host python3",
        f"{text} is older than {MIN_PYTHON[0]}.{MIN_PYTHON[1]}",
        "Install a newer python3 (brew install python@3.12) and put it on PATH.",
    )
    return False


def create_venv(report, venv_dir, interpreter):
    print(f"creating capture venv at {venv_dir}", flush=True)
    result = subprocess.run(
        [sys.executable, "-m", "venv", venv_dir],
        check=False,
    )
    if result.returncode != 0 or not os.path.exists(interpreter):
        report.fail(
            "capture venv",
            f"could not create {venv_dir}",
            f"{sys.executable} -m venv {venv_dir}  # run manually to see why",
        )
        return False
    return True


def install_requirements(report, interpreter, requirements_path, missing):
    print(
        f"installing {', '.join(missing)} into the capture venv "
        f"(from {requirements_path})",
        flush=True,
    )
    result = subprocess.run(
        [
            interpreter,
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "-r",
            requirements_path,
        ],
        check=False,
    )
    if result.returncode != 0:
        report.fail(
            "capture venv packages",
            f"pip install exited {result.returncode} (offline? proxy?)",
            f"{interpreter} -m pip install -r {requirements_path}",
        )
        return False
    return True


def ensure_packages(report, interpreter, requirements_path, modules, allow_install):
    probe, error = run_probe(interpreter, PROBE_IMPORTS, modules)
    if probe is None:
        report.fail(
            "python packages",
            f"could not probe {interpreter}: {error}",
            f"{interpreter} -c 'import numpy, matplotlib'  # run manually to see why",
        )
        return False
    missing = [name for name, result in probe.items() if not result["ok"]]
    if missing and allow_install:
        if not install_requirements(report, interpreter, requirements_path, missing):
            return False
        probe, error = run_probe(interpreter, PROBE_IMPORTS, modules)
        if probe is None:
            report.fail(
                "python packages",
                f"could not probe {interpreter} after install: {error}",
                f"{interpreter} -m pip install -r {requirements_path}",
            )
            return False
        missing = [name for name, result in probe.items() if not result["ok"]]

    for name, result in probe.items():
        if result["ok"]:
            report.ok(f"python module {name}", result["detail"])
        else:
            remedy = (
                f"{interpreter} -m pip install -r {requirements_path}"
                if allow_install
                else "Drop --no-venv so the capture venv can provide it, or "
                f"install it into {interpreter} yourself."
            )
            report.fail(f"python module {name}", result["detail"], remedy)
    return not missing


def check_ffmpeg(report):
    path = shutil.which("ffmpeg")
    if path is None:
        report.fail("ffmpeg", "not found on PATH", "brew install ffmpeg")
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
    report.ok("ffmpeg", result.stdout.splitlines()[0] if result.stdout else path)


def check_matplotlib_writer(report, interpreter):
    probe, error = run_probe(interpreter, PROBE_WRITER)
    if probe is None:
        report.fail(
            "matplotlib FFMpegWriter",
            f"probe failed: {error}",
            "Check that matplotlib imports cleanly in the capture venv.",
        )
        return
    if probe["ok"]:
        report.ok("matplotlib FFMpegWriter", "available")
        return
    report.fail(
        "matplotlib FFMpegWriter",
        probe["detail"] or "matplotlib cannot find an ffmpeg it can drive",
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
        "--venv",
        default="",
        help="virtualenv directory to create/reuse; required unless --no-venv",
    )
    parser.add_argument(
        "--requirements",
        default="",
        help="requirements file installed into the venv",
    )
    parser.add_argument(
        "--no-venv",
        action="store_true",
        help="use the host interpreter as-is and never install anything",
    )
    parser.add_argument(
        "--recreate-venv",
        action="store_true",
        help="delete and rebuild the venv before checking it",
    )
    parser.add_argument("--output-dir", default="")
    parser.add_argument("--error-log", default="")
    parser.add_argument(
        "--write-interpreter",
        default="",
        help="write the ready-to-use interpreter path here on success",
    )
    args = parser.parse_args()

    report = Report()
    interpreter = sys.executable
    healthy = check_host_python(report)

    modules, requirements_path = [], args.requirements
    if requirements_path:
        if not os.path.exists(requirements_path):
            report.fail(
                "requirements file",
                f"{requirements_path} does not exist",
                "Pass --requirements=tools/capture/requirements.txt.",
            )
            healthy = False
        else:
            _, modules = read_requirements(requirements_path)

    if healthy and args.no_venv:
        report.ok("capture venv", "disabled (--no-venv), using the host interpreter")
        if modules:
            healthy = ensure_packages(
                report, interpreter, requirements_path, modules, allow_install=False
            )
    elif healthy:
        if not args.venv:
            report.fail(
                "capture venv",
                "no --venv path given",
                "Pass --venv=<dir>, or --no-venv to use the host interpreter.",
            )
            healthy = False
        else:
            venv_dir = os.path.abspath(args.venv)
            venv_python = os.path.join(venv_dir, "bin", "python")
            if args.recreate_venv and os.path.isdir(venv_dir):
                shutil.rmtree(venv_dir)
            if not os.path.exists(venv_python):
                healthy = create_venv(report, venv_dir, venv_python)
            if healthy:
                interpreter = venv_python
                report.ok("capture venv", venv_dir)
                # The host interpreter is externally managed on macOS; every
                # package lands here instead.
                report.note(f"packages install into {venv_dir}, never into "
                            f"{sys.executable}")
                if modules:
                    healthy = ensure_packages(
                        report,
                        interpreter,
                        requirements_path,
                        modules,
                        allow_install=True,
                    )

    check_ffmpeg(report)
    if healthy:
        check_matplotlib_writer(report, interpreter)
    check_output_dir(report, args.output_dir)

    text = report.text()
    sys.stdout.write(text)
    sys.stdout.flush()
    if report.failures:
        if args.error_log:
            try:
                os.makedirs(os.path.dirname(args.error_log) or ".", exist_ok=True)
                with open(args.error_log, "w", encoding="utf-8") as handle:
                    handle.write(text)
                sys.stderr.write(f"preflight error log: {args.error_log}\n")
            except OSError as error:
                sys.stderr.write(f"could not write {args.error_log}: {error}\n")
        return 1

    if args.write_interpreter:
        with open(args.write_interpreter, "w", encoding="utf-8") as handle:
            handle.write(interpreter + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
