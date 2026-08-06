#!/usr/bin/env python3
"""Renders a transform capture CSV to an mp4.

Reads either CSV the capture writers produce (see
engine/src/tests/capture/transform_capture.h) and picks the view from the data,
so a new capture does not need a new renderer:

  * hierarchy CSV with at least one parented node (a skeleton) -> 3D animation
    of the node graph with a follow camera, drawn from world_px/py/pz;
  * hierarchy CSV with no parents, or an entity CSV (root_px/py/pz) -> top-down
    XZ path plot, one moving marker and trail per subject.

The node index/name columns are detected rather than hard-coded, so
`bone_index`/`bone_name` and `entity_index`/`entity_name` both work.

Engine coordinates are Y-up: (x, y, z) maps to plot axes X=forward x,
Y=lateral z, Z=height y.
"""
import argparse
import csv
import sys

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.animation import FFMpegWriter, FuncAnimation  # noqa: E402


class Capture:
    """Samples of one or more nodes, in world space."""

    def __init__(self):
        self.samples = []          # ordered sample ids
        self.positions = None      # (T, N, 3) world positions
        self.names = []            # per-node label
        self.parents = []          # per-node parent index, -1 for roots
        self.times = []            # per-sample seconds

    @property
    def is_hierarchy(self):
        return any(parent >= 0 for parent in self.parents)


def _detect_columns(fieldnames):
    index_column = None
    name_column = None
    for field in fieldnames:
        if field == "parent_index":
            continue
        if index_column is None and field.endswith("_index"):
            index_column = field
        if name_column is None and field.endswith("_name"):
            name_column = field
    return index_column, name_column


def load(path):
    with open(path, newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit(f"{path}: no rows")
    fields = list(rows[0].keys())

    capture = Capture()
    by_sample = {}
    times = {}

    if "world_px" in fields:
        index_column, name_column = _detect_columns(fields)
        if index_column is None:
            raise SystemExit(f"{path}: no node index column (expected *_index)")
        names, parents = {}, {}
        for row in rows:
            sample = int(row["sample"])
            node = int(row[index_column])
            by_sample.setdefault(sample, {})[node] = (
                float(row["world_px"]),
                float(row["world_py"]),
                float(row["world_pz"]),
            )
            names[node] = row.get(name_column, str(node)) if name_column else str(node)
            parents[node] = int(row.get("parent_index", -1))
            times[sample] = float(row.get("time_seconds", 0.0))
        node_ids = sorted(names)
    elif "root_px" in fields:
        # Entity series: every net_id is an independent, unparented subject.
        names, parents = {}, {}
        for row in rows:
            sample = int(row["sample"])
            net_id = int(row["net_id"])
            by_sample.setdefault(sample, {})[net_id] = (
                float(row["root_px"]),
                float(row["root_py"]),
                float(row["root_pz"]),
            )
            names[net_id] = f"net {net_id}"
            parents[net_id] = -1
            times[sample] = float(row.get("time_seconds", 0.0))
        node_ids = sorted(names)
    else:
        raise SystemExit(f"{path}: unrecognized capture schema")

    capture.samples = sorted(by_sample)
    capture.names = [names[node] for node in node_ids]
    capture.parents = [parents[node] for node in node_ids]
    capture.times = [times[sample] for sample in capture.samples]

    # Nodes that vanish mid-capture would break the fixed-size array; require a
    # complete series so a malformed capture is reported instead of plotted.
    for sample in capture.samples:
        missing = [node for node in node_ids if node not in by_sample[sample]]
        if missing:
            raise SystemExit(
                f"{path}: sample {sample} is missing node(s) {missing[:4]}"
            )
    capture.positions = np.array(
        [[by_sample[sample][node] for node in node_ids] for sample in capture.samples]
    )
    # Parent indices address node ids; remap them to array positions.
    position_of = {node: index for index, node in enumerate(node_ids)}
    capture.parents = [
        position_of.get(parent, -1) if parent >= 0 else -1
        for parent in capture.parents
    ]
    return capture


def render_hierarchy(capture, out_path, title, color, fps, window, highlight_suffix):
    positions = capture.positions
    node_count = positions.shape[1]
    forward, height, lateral = positions[..., 0], positions[..., 1], positions[..., 2]
    edges = [
        (parent, child)
        for child, parent in enumerate(capture.parents)
        if parent >= 0
    ]
    highlighted = {
        index
        for index, name in enumerate(capture.names)
        if highlight_suffix and name.endswith(highlight_suffix)
    }

    figure = plt.figure(figsize=(11, 7))
    axes = figure.add_subplot(111, projection="3d")

    # Follow camera: a fixed-size window tracking the root keeps the gait large;
    # forward progress shows through the scrolling grid and the root trail.
    root_path = positions[:, 0, :]
    lateral_low, lateral_high = lateral.min() - 2.5, lateral.max() + 2.5
    height_low, height_high = height.min() - 1.5, height.max() + 1.5
    axes.set_box_aspect(
        (
            2 * window,
            max(lateral_high - lateral_low, 3),
            max(height_high - height_low, 3),
        )
    )

    edge_lines = [
        axes.plot([], [], [], color=color, lw=2.0, alpha=0.9)[0] for _ in edges
    ]
    body_points = axes.plot([], [], [], "o", color=color, ms=4.5)[0]
    highlight_points = axes.plot([], [], [], "o", color="#e34a33", ms=8)[0]
    trail = axes.plot([], [], [], "-", color="#f0a030", lw=1.4, alpha=0.9)[0]
    heading = axes.set_title("")

    def setup():
        axes.set_ylim(lateral_low, lateral_high)
        axes.set_zlim(height_low, height_high)
        axes.set_xlabel("X forward (m)")
        axes.set_ylabel("Z lateral (m)")
        axes.set_zlabel("Y height (m)")
        axes.view_init(elev=16, azim=-68)
        return []

    def update(frame_index):
        points = positions[frame_index]
        center = root_path[frame_index, 0]
        axes.set_xlim(center - window, center + window)
        for line, (parent, child) in zip(edge_lines, edges):
            line.set_data(
                [points[parent, 0], points[child, 0]],
                [points[parent, 2], points[child, 2]],
            )
            line.set_3d_properties([points[parent, 1], points[child, 1]])
        body = [index for index in range(node_count) if index not in highlighted]
        body_points.set_data(points[body, 0], points[body, 2])
        body_points.set_3d_properties(points[body, 1])
        marked = sorted(highlighted)
        highlight_points.set_data(points[marked, 0], points[marked, 2])
        highlight_points.set_3d_properties(points[marked, 1])
        visible = root_path[: frame_index + 1, 0] >= center - window
        trail.set_data(
            root_path[: frame_index + 1, 0][visible],
            root_path[: frame_index + 1, 2][visible],
        )
        trail.set_3d_properties(root_path[: frame_index + 1, 1][visible])
        heading.set_text(
            f"{title}\nsample {capture.samples[frame_index]:4d}/"
            f"{len(capture.samples)}   t={capture.times[frame_index]:6.2f}s   "
            f"rootX={center:7.2f}m   rootY={root_path[frame_index, 1]:7.2f}m"
        )
        return []

    save(figure, update, setup, len(capture.samples), out_path, title, fps)


def render_paths(capture, out_path, title, color, fps):
    positions = capture.positions
    node_count = positions.shape[1]
    forward, lateral = positions[..., 0], positions[..., 2]

    figure = plt.figure(figsize=(10, 7))
    axes = figure.add_subplot(111)
    palette = plt.get_cmap("tab10")

    trails, markers = [], []
    for index in range(node_count):
        node_color = color if node_count == 1 else palette(index % 10)
        trails.append(
            axes.plot([], [], "-", color=node_color, lw=1.6, alpha=0.9,
                      label=capture.names[index])[0]
        )
        markers.append(axes.plot([], [], "o", color=node_color, ms=7)[0])
    heading = axes.set_title("")

    def setup():
        pad = 1.0
        axes.set_xlim(forward.min() - pad, forward.max() + pad)
        axes.set_ylim(lateral.min() - pad, lateral.max() + pad)
        axes.set_xlabel("X forward (m)")
        axes.set_ylabel("Z lateral (m)")
        axes.set_aspect("equal", adjustable="box")
        axes.grid(True, alpha=0.3)
        if node_count > 1:
            axes.legend(loc="upper right", fontsize=8)
        return []

    def update(frame_index):
        for index in range(node_count):
            trails[index].set_data(
                forward[: frame_index + 1, index], lateral[: frame_index + 1, index]
            )
            markers[index].set_data(
                [forward[frame_index, index]], [lateral[frame_index, index]]
            )
        heading.set_text(
            f"{title}\nsample {capture.samples[frame_index]:4d}/"
            f"{len(capture.samples)}   t={capture.times[frame_index]:6.2f}s"
        )
        return []

    save(figure, update, setup, len(capture.samples), out_path, title, fps)


def save(figure, update, setup, frames, out_path, title, fps):
    animation = FuncAnimation(
        figure, update, init_func=setup, frames=frames, blit=False
    )
    writer = FFMpegWriter(fps=fps, bitrate=4000, metadata={"title": title})
    animation.save(out_path, writer=writer, dpi=110)
    plt.close(figure)
    print(f"wrote {out_path} ({frames} frames @ {fps} fps)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", required=True, help="capture CSV to render")
    parser.add_argument("--out", required=True, help="output mp4 path")
    parser.add_argument("--title", default="capture")
    parser.add_argument("--color", default="#2b8cbe")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument(
        "--window",
        type=float,
        default=7.0,
        help="half-width of the follow camera window, metres (hierarchy view)",
    )
    parser.add_argument(
        "--highlight-suffix",
        default="_Foot",
        help="nodes whose name ends with this are drawn as contact markers",
    )
    args = parser.parse_args()

    capture = load(args.csv)
    if capture.is_hierarchy:
        render_hierarchy(
            capture,
            args.out,
            args.title,
            args.color,
            args.fps,
            args.window,
            args.highlight_suffix,
        )
    else:
        render_paths(capture, args.out, args.title, args.color, args.fps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
