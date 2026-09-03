#!/usr/bin/env python3
"""Fails when a package's Bazel dependencies cross a layering boundary.

The layering is stated in docs/GAME_SERVER_V1.md and
docs/AI_FRAMEWORK_REFACTOR_REQUIREMENTS.md: the kernel owns entity storage,
ticking, transport, snapshots and relevance; game_server owns gameplay
decisions; engine/components/ai owns decision abstractions and depends on
neither.

The compiler already enforces most of this -- game_server cannot include
world/public/components.h today because that header is not on its include
path. What the compiler cannot object to is somebody adding the dependency,
at which point the include starts working and the boundary is gone with no
diagnostic anywhere. That edit is what this checks.

Deliberately an allowlist rather than a blocklist. A blocklist has to predict
the target somebody will reach for; an allowlist makes any new engine
dependency a decision somebody states in this file, which is the point.

What this cannot check is the other direction: a gameplay concept added to
engine/src/world/public/components.h or to the kernel ABI. That is not a
dependency edge, so nothing here would see it -- see the review criterion in
docs/GAME_SERVER_V1.md, which is deliberately a criterion rather than a word
blocklist with a baseline that whoever is about to break the rule would edit.
"""

import os
import re
import sys

# package -> (allowed first-party dependency prefixes, why)
ALLOWED = {
    "game_server": (
        (
            "//engine/components/ai",
            "//engine/src/kernel:kernel_api",
            "//engine/src/kernel:kernel_api_internal",
            "//game_server",
        ),
        "game_server talks to the kernel through the public server-side C API "
        "only. Depending on //engine/src/world or //engine/src/simulation "
        "would put gameplay on the wrong side of the boundary, and would make "
        "kernel components includable from gameplay code.",
    ),
    "engine/components/ai": (
        ("//engine/components/ai",),
        "engine/components/ai owns decision abstractions and must not know "
        "about the kernel, the world, the simulation, game_server or the app. "
        "It is the one library here whose isolation is load-bearing for the "
        "AI framework's ownership model.",
    ),
}

# Rules whose dependencies are exempt. A test may link the whole kernel: it is
# not shipping a boundary, it is standing a real one up to test against.
EXEMPT_RULE_KINDS = {"cc_test", "py_test", "sh_test"}

RULE_START = re.compile(r"^([a-z_]+)\($")
NAME = re.compile(r'^\s*name = "([^"]+)",\s*$')
LABEL = re.compile(r'"(//[^"]*)"')


def parse_rules(text):
    """Yields (kind, name, [deps]). Relies on buildifier formatting: a rule
    opens with `kind(` at column zero and closes with `)` at column zero."""
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        start = RULE_START.match(lines[index])
        if start is None:
            index += 1
            continue
        kind = start.group(1)
        body = []
        index += 1
        while index < len(lines) and lines[index] != ")":
            body.append(lines[index])
            index += 1
        name = None
        deps = []
        in_deps = False
        for line in body:
            matched = NAME.match(line)
            if matched is not None and name is None:
                name = matched.group(1)
            stripped = line.strip()
            if stripped.startswith("deps = ["):
                in_deps = True
                deps.extend(LABEL.findall(stripped))
                if stripped.endswith("]") or stripped.endswith("],"):
                    in_deps = False
                continue
            if in_deps:
                if stripped.startswith("]"):
                    in_deps = False
                    continue
                deps.extend(LABEL.findall(stripped))
        if name is not None:
            yield kind, name, deps


def main():
    root = os.environ.get("TEST_SRCDIR")
    workspace = os.environ.get("TEST_WORKSPACE")
    base = os.path.join(root, workspace) if root and workspace else "."

    failures = []
    for package, (allowed, reason) in ALLOWED.items():
        build_path = os.path.join(base, package, "BUILD.bazel")
        if not os.path.exists(build_path):
            failures.append(
                "cannot read {}: this check is only as good as the files it "
                "can see".format(build_path))
            continue
        with open(build_path, "r") as stream:
            rules = list(parse_rules(stream.read()))
        if not rules:
            # A parse that finds nothing would pass every rule below, which is
            # the one failure mode a checker must never have.
            failures.append(
                "parsed no rules out of {}: the format this reads has "
                "changed and the check is no longer checking "
                "anything".format(build_path))
            continue
        for kind, name, deps in rules:
            if kind in EXEMPT_RULE_KINDS:
                continue
            for dep in deps:
                if any(dep.startswith(prefix) for prefix in allowed):
                    continue
                failures.append(
                    "//{}:{} ({}) depends on {}\n    {}\n    Allowed: {}\n"
                    "    If this dependency is right, the boundary has moved: "
                    "say so in tools/check_layering_boundaries.py and in "
                    "docs/GAME_SERVER_V1.md.".format(
                        package, name, kind, dep, reason, ", ".join(allowed)))

    if failures:
        print("layering boundary violated:\n", file=sys.stderr)
        for failure in failures:
            print("  " + failure + "\n", file=sys.stderr)
        return 1
    print("layering boundaries hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
