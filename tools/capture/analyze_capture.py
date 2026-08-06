#!/usr/bin/env python3
"""Summarizes a transform capture and diffs two captures of the same run.

Section 1 describes the recorded path and checks the capture is well formed
(no NaN/Inf, every sample carries every node, unit quaternions). Section 2 is
the layer-1 parity gate: the native and plugin runs differ only by which dylib
and bundle were loaded, so their raw output must agree within tolerance.

Writes the report to --out and exits non-zero when a check fails.
"""
import argparse
import csv
import math
import sys

POSITION_TOLERANCE = 1e-5      # metres, local/root positions
WORLD_TOLERANCE = 1e-3         # metres, FK composite positions
SCALE_TOLERANCE = 1e-5
QUATERNION_DOT_TOLERANCE = 1e-6  # 1 - |dot|


def load_root(path):
    with open(path, newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_nodes(path):
    rows = {}
    index_column = None
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for field in reader.fieldnames or []:
            if field != "parent_index" and field.endswith("_index"):
                index_column = field
                break
        if index_column is None:
            raise SystemExit(f"{path}: no node index column (expected *_index)")
        for row in reader:
            rows[(int(row["sample"]), int(row[index_column]))] = row
    return rows


def number(row, key):
    return float(row[key])


def finite(*values):
    return all(math.isfinite(value) for value in values)


def quaternion_dot(lhs, rhs):
    return sum(left * right for left, right in zip(lhs, rhs))


def quaternion(row, prefix):
    return [number(row, prefix + axis) for axis in ("qx", "qy", "qz", "qw")]


class Report:
    def __init__(self):
        self.lines = []
        self.failures = []

    def line(self, text=""):
        self.lines.append(text)

    def check(self, name, passed, detail):
        self.lines.append(f"  [{'PASS' if passed else 'FAIL'}] {name}: {detail}")
        if not passed:
            self.failures.append(f"{name}: {detail}")

    def text(self):
        lines = list(self.lines)
        lines.append("")
        if self.failures:
            lines.append(f"VERDICT: FAIL ({len(self.failures)} check(s))")
            lines.extend(f"  - {failure}" for failure in self.failures)
        else:
            lines.append("VERDICT: PASS")
        return "\n".join(lines) + "\n"


def describe_run(report, label, root_rows, node_rows, expected_samples):
    report.line(f"=== {label} run ===")
    samples = sorted({int(row["sample"]) for row in root_rows})
    report.line(f"  samples: {len(samples)} (requested {expected_samples})")
    if not root_rows:
        report.check("row count", False, "root CSV is empty")
        return
    report.check(
        "sample count",
        len(samples) == expected_samples,
        f"{len(samples)} recorded",
    )

    first, last = root_rows[0], root_rows[-1]
    x0, y0, z0 = (number(first, key) for key in ("root_px", "root_py", "root_pz"))
    x1, y1, z1 = (number(last, key) for key in ("root_px", "root_py", "root_pz"))
    xs = [number(row, "root_px") for row in root_rows]
    ys = [number(row, "root_py") for row in root_rows]
    zs = [number(row, "root_pz") for row in root_rows]
    planar = math.hypot(x1 - x0, z1 - z0)
    report.line(f"  start:        ({x0:.3f}, {y0:.3f}, {z0:.3f})")
    report.line(f"  end:          ({x1:.3f}, {y1:.3f}, {z1:.3f})")
    report.line(
        f"  displacement: dX {x1 - x0:+.3f} m  dZ {z1 - z0:+.3f} m  "
        f"planar {planar:.3f} m"
    )
    report.line(f"  X range:      [{min(xs):.3f}, {max(xs):.3f}]")
    report.line(f"  Z range:      [{min(zs):.3f}, {max(zs):.3f}]")
    report.line(f"  Y range:      [{min(ys):.3f}, {max(ys):.3f}]")

    travelled = sum(
        math.hypot(
            number(b, "root_px") - number(a, "root_px"),
            number(b, "root_pz") - number(a, "root_pz"),
        )
        for a, b in zip(root_rows, root_rows[1:])
    )
    report.line(f"  path length:  {travelled:.3f} m")

    bad_root = sum(
        1
        for row in root_rows
        if not finite(
            *(
                number(row, key)
                for key in (
                    "root_px", "root_py", "root_pz",
                    "root_qx", "root_qy", "root_qz", "root_qw",
                    "velocity_x", "velocity_y", "velocity_z",
                )
            )
        )
    )
    report.check("root values finite", bad_root == 0, f"{bad_root} bad row(s)")

    counts = {}
    bad_nodes = 0
    degenerate_quaternions = 0
    for (sample, _node), row in node_rows.items():
        counts[sample] = counts.get(sample, 0) + 1
        values = [
            number(row, key)
            for key in (
                "local_px", "local_py", "local_pz",
                "local_qx", "local_qy", "local_qz", "local_qw",
                "local_sx", "local_sy", "local_sz",
                "world_px", "world_py", "world_pz",
            )
        ]
        if not finite(*values):
            bad_nodes += 1
        norm = math.sqrt(sum(component * component for component in values[3:7]))
        if norm < 1e-3:
            degenerate_quaternions += 1
    distinct_counts = sorted(set(counts.values()))
    report.check(
        "node count per sample",
        len(distinct_counts) == 1,
        f"{distinct_counts}",
    )
    report.check("node values finite", bad_nodes == 0, f"{bad_nodes} bad row(s)")
    report.check(
        "quaternions non-degenerate",
        degenerate_quaternions == 0,
        f"{degenerate_quaternions} bad row(s)",
    )
    report.line()


def compare_runs(report, native_root, plugin_root, native_nodes, plugin_nodes):
    report.line("=== native <-> plugin parity (layer 1) ===")
    if len(native_root) != len(plugin_root):
        report.check(
            "root row count",
            False,
            f"native {len(native_root)} vs plugin {len(plugin_root)}",
        )
        return
    if set(native_nodes) != set(plugin_nodes):
        report.check(
            "node key set",
            False,
            f"native {len(native_nodes)} vs plugin {len(plugin_nodes)} rows",
        )
        return

    max_root_position = 0.0
    min_root_dot = 1.0
    tick_mismatches = 0
    for native, plugin in zip(native_root, plugin_root):
        for key in ("root_px", "root_py", "root_pz"):
            max_root_position = max(
                max_root_position, abs(number(native, key) - number(plugin, key))
            )
        min_root_dot = min(
            min_root_dot,
            abs(quaternion_dot(quaternion(native, "root_"), quaternion(plugin, "root_"))),
        )
        if native["tick"] != plugin["tick"]:
            tick_mismatches += 1

    max_local_position = 0.0
    max_scale = 0.0
    max_world_position = 0.0
    min_node_dot = 1.0
    for key, native in native_nodes.items():
        plugin = plugin_nodes[key]
        for column in ("local_px", "local_py", "local_pz"):
            max_local_position = max(
                max_local_position,
                abs(number(native, column) - number(plugin, column)),
            )
        for column in ("local_sx", "local_sy", "local_sz"):
            max_scale = max(
                max_scale, abs(number(native, column) - number(plugin, column))
            )
        for column in ("world_px", "world_py", "world_pz"):
            max_world_position = max(
                max_world_position,
                abs(number(native, column) - number(plugin, column)),
            )
        min_node_dot = min(
            min_node_dot,
            abs(
                quaternion_dot(
                    quaternion(native, "local_"), quaternion(plugin, "local_")
                )
            ),
        )

    max_angle_degrees = math.degrees(
        2.0 * math.acos(max(-1.0, min(1.0, min_node_dot)))
    )
    report.check(
        "root position",
        max_root_position <= POSITION_TOLERANCE,
        f"max delta {max_root_position:.3e} m (tolerance {POSITION_TOLERANCE:g})",
    )
    report.check(
        "root rotation",
        1.0 - min_root_dot <= QUATERNION_DOT_TOLERANCE,
        f"min |dot| {min_root_dot:.9f}",
    )
    report.check(
        "node local position",
        max_local_position <= POSITION_TOLERANCE,
        f"max delta {max_local_position:.3e} m",
    )
    report.check(
        "node scale",
        max_scale <= SCALE_TOLERANCE,
        f"max delta {max_scale:.3e}",
    )
    report.check(
        "node local rotation",
        1.0 - min_node_dot <= QUATERNION_DOT_TOLERANCE,
        f"min |dot| {min_node_dot:.9f} ({max_angle_degrees:.3f} deg)",
    )
    report.check(
        "node world position",
        max_world_position <= WORLD_TOLERANCE,
        f"max delta {max_world_position:.3e} m (tolerance {WORLD_TOLERANCE:g})",
    )
    report.check("pose tick alignment", tick_mismatches == 0, f"{tick_mismatches} mismatch(es)")
    report.line()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-prefix", required=True)
    parser.add_argument("--plugin-prefix", default="")
    parser.add_argument("--out", required=True)
    parser.add_argument("--samples", type=int, required=True)
    parser.add_argument("--tick-rate", type=int, default=30)
    parser.add_argument("--path", default="")
    args = parser.parse_args()

    report = Report()
    report.line("Locomotion capture report")
    report.line(
        f"  path:      {args.path or '(default)'}\n"
        f"  sampling:  {args.samples} samples @ {args.tick_rate} Hz = "
        f"{args.samples / args.tick_rate:.2f} s"
    )
    report.line()

    native_root = load_root(f"{args.native_prefix}_root.csv")
    native_nodes = load_nodes(f"{args.native_prefix}_bones.csv")
    describe_run(report, "native", native_root, native_nodes, args.samples)

    if args.plugin_prefix:
        plugin_root = load_root(f"{args.plugin_prefix}_root.csv")
        plugin_nodes = load_nodes(f"{args.plugin_prefix}_bones.csv")
        describe_run(report, "plugin", plugin_root, plugin_nodes, args.samples)
        compare_runs(report, native_root, plugin_root, native_nodes, plugin_nodes)
    else:
        report.line("=== native <-> plugin parity (layer 1) ===")
        report.line("  skipped (--skip-plugin)")
        report.line()

    text = report.text()
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(text)
    sys.stdout.write(text)
    print(f"wrote {args.out}")
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
