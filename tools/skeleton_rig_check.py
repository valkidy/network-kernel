#!/usr/bin/env python3
"""Checks a locomotion rig for the failures that load-time validation misses.

The kernel already rejects a rig whose knee is not a child of its hip, whose
three leg joints collide, or whose mid_axis_local is more than 60 degrees off
the bind-pose bend plane. What it cannot see is a rig that is technically valid
but numerically miserable: a limb so nearly straight that the IK has no stable
bend direction, a pole vector that lies along the very axis it is meant to
disambiguate, a hinge that clears the 0.5 threshold by so little that a small
authoring change will drop it below, or a hinge pointing the wrong way.

The validator ignores the hinge's sign, but IKTwoBoneJob does not: a positive
rotation about mid_axis opens the knee. A leg whose axis points along the bind
bend normal cross(knee - hip, foot - knee) is folded backwards, and the pole
then spins the whole limb 180 degrees about its length to point the knee where
it asked. A box limb looks the same either way; a skinned foot faces backwards.

Reads manifest_version 2 (see //tools:ozz_skeleton_manifest) for the rest pose
and the authored leg parameters from either a rig_version 2 .rig.yaml (--rig)
or, for older rigs, the entity template (--template):

    python3 tools/skeleton_rig_check.py \
        --manifest=bazel-bin/game_server/gameplay_catalog/skeleton_assets/generated/simplified_gingerbread_giant.skeleton_manifest.json \
        --rig=game_server/gameplay_catalog/skeleton_assets/raw/simplified_gingerbread_giant.rig.yaml

    python3 tools/skeleton_rig_check.py \
        --manifest=bazel-bin/game_server/gameplay_catalog/skeleton_assets/generated/simplified_tripod.skeleton_manifest.json \
        --template=game_server/gameplay_catalog/entity_templates/22_tripod_actor.yaml

Exits non-zero if any check fails. Warnings alone do not fail the run.
"""

import argparse
import json
import math
import sys

# The kernel rejects below this; see validate_locomotion_rig.
HINGE_REJECT = 0.5
# Below this it passes but has little room for the rig to change.
HINGE_WARN = 0.70
# A limb resting this close to full extension has almost no dip budget before
# the IK clamps, which reads as a straight leg skating over terrain.
REST_EXTENSION_WARN = 0.97
# Two joints closer than this are effectively coincident at these rig scales.
MIN_BONE_LENGTH = 1e-3
# |cos| between pole and the hip->foot axis; at 1.0 the pole cannot pick a side.
POLE_COLLINEAR_WARN = 0.95


def quat_matrix(q):
    x, y, z, w = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


def mat_apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def mat_transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def sub(a, b):
    return [p - q for p, q in zip(a, b)]


def dot(a, b):
    return sum(p * q for p, q in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def length(a):
    return math.sqrt(dot(a, a))


def unit(a):
    n = length(a)
    return [v / n for v in a] if n > 1e-12 else [0.0, 0.0, 0.0]


def model_pose(bones):
    """Model-space position and rotation basis per bone index."""
    positions = {}
    rotations = {}
    for bone in bones:
        index = bone["index"]
        local_rotation = quat_matrix(bone["rest_rotation"])
        local_translation = bone["rest_translation"]
        parent = bone["parent_index"]
        if parent < 0:
            rotations[index] = local_rotation
            positions[index] = list(local_translation)
        else:
            rotations[index] = mat_mul(rotations[parent], local_rotation)
            positions[index] = [
                p + o for p, o in zip(
                    positions[parent],
                    mat_apply(rotations[parent], local_translation))]
    return positions, rotations


def load_template_legs(path):
    """Minimal reader for the leg block, to avoid a PyYAML dependency."""
    legs = []
    current = None
    in_legs = False
    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n")
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if stripped.startswith("legs:") and indent == 2:
            in_legs = True
            continue
        if in_legs and indent <= 2 and not stripped.startswith("-"):
            break
        if not in_legs:
            continue
        if stripped.startswith("- id:"):
            current = {"id": stripped.split(":", 1)[1].strip()}
            legs.append(current)
            continue
        if current is None or ":" not in stripped:
            continue
        key, value = stripped.split(":", 1)
        key = key.lstrip("- ").strip()
        value = value.strip()
        if value.startswith("{"):
            parts = {}
            for item in value.strip("{}").split(","):
                axis, number = item.split(":")
                parts[axis.strip()] = float(number)
            current[key] = [parts.get("x", 0.0), parts.get("y", 0.0),
                            parts.get("z", 0.0)]
        else:
            current[key] = value
    return legs


def parse_flow_map(text):
    """{x: 1.0, y: 0.0, z: 0.0} -> [1.0, 0.0, 0.0]"""
    parts = {}
    for item in text.strip().strip("{}").split(","):
        axis, number = item.split(":")
        parts[axis.strip()] = float(number)
    return [parts.get("x", 0.0), parts.get("y", 0.0), parts.get("z", 0.0)]


def load_rig_legs(path):
    """Leg block of a rig_version 2 .rig.yaml, in the template reader's keys.

    Its legs are flow mappings that may wrap over several lines, and the knee
    hinge is shared by every leg.
    """
    text = open(path, encoding="utf-8").read()
    hinge = None
    legs = []
    entry = None
    in_legs = False
    for raw in text.splitlines():
        stripped = raw.split("#", 1)[0].strip()
        if not stripped:
            continue
        if stripped.startswith("knee_hinge_local:"):
            hinge = parse_flow_map(stripped.split(":", 1)[1])
            continue
        if stripped == "legs:":
            in_legs = True
            continue
        if not in_legs:
            continue
        if entry is None:
            if not stripped.startswith("- {"):
                break
            entry = stripped[2:]
        else:
            entry += " " + stripped
        if entry.count("{") == entry.count("}"):
            body = entry.strip()[1:-1]
            pole = None
            if "pole_local:" in body:
                body, pole_text = body.split("pole_local:", 1)
                pole = parse_flow_map(pole_text)
            fields = {}
            for item in body.split(","):
                if ":" in item:
                    key, value = item.split(":", 1)
                    fields[key.strip()] = value.strip()
            leg = {
                "id": fields.get("id"),
                "hip_bone": fields.get("hip"),
                "knee_bone": fields.get("knee"),
                "foot_bone": fields.get("foot"),
            }
            if pole is not None:
                leg["pole_local"] = pole
            if hinge is not None:
                leg["mid_axis_local"] = hinge
            legs.append(leg)
            entry = None
    return legs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--rig")
    source.add_argument("--template")
    args = parser.parse_args()

    manifest = json.load(open(args.manifest, encoding="utf-8"))
    if manifest.get("manifest_version") != 2:
        print(f"rig check needs manifest_version 2, got "
              f"{manifest.get('manifest_version')}", file=sys.stderr)
        return 2

    bones = manifest["bones"]
    index_of = {bone["name"]: bone["index"] for bone in bones}
    positions, rotations = model_pose(bones)
    legs_path = args.rig or args.template
    legs = load_rig_legs(legs_path) if args.rig else load_template_legs(legs_path)
    if not legs:
        print(f"no legs found in {legs_path}", file=sys.stderr)
        return 2

    errors = []
    warnings = []
    print(f"{manifest['name']}: {len(legs)} legs, {manifest['bone_count']} bones")

    for leg in legs:
        name = leg["id"]
        try:
            hip_index = index_of[leg["hip_bone"]]
            knee_index = index_of[leg["knee_bone"]]
            foot_index = index_of[leg["foot_bone"]]
        except KeyError as missing:
            errors.append(f"{name}: bone {missing} is not in the manifest")
            continue

        hip = positions[hip_index]
        knee = positions[knee_index]
        foot = positions[foot_index]
        upper = length(sub(knee, hip))
        lower = length(sub(foot, knee))
        reach = upper + lower
        rest = length(sub(foot, hip))

        if upper < MIN_BONE_LENGTH or lower < MIN_BONE_LENGTH:
            errors.append(
                f"{name}: degenerate bone length (upper={upper:.4f} "
                f"lower={lower:.4f}); the IK has no limb to bend")
            continue

        extension = rest / reach if reach > 0.0 else 1.0
        hinge_model = cross(sub(knee, hip), sub(foot, knee))
        if length(hinge_model) <= 1e-6:
            errors.append(
                f"{name}: hip, knee and foot are collinear in the bind pose, "
                f"so there is no bend plane and no stable fold direction")
            continue

        # mid_axis_local lives in the knee's frame, so the hinge has to be
        # rotated into it before the two can be compared.
        hinge = unit(mat_apply(mat_transpose(rotations[knee_index]),
                               hinge_model))
        authored_axis = unit(leg.get("mid_axis_local", [0.0, 0.0, 1.0]))
        signed_alignment = dot(hinge, authored_axis)
        alignment = abs(signed_alignment)

        pole = unit(leg.get("pole_local", [0.0, 0.0, 1.0]))
        limb_axis = unit(sub(foot, hip))
        pole_alignment = abs(dot(pole, limb_axis))

        status = "ok"
        if alignment < HINGE_REJECT:
            errors.append(
                f"{name}: mid_axis_local is {alignment:.3f} against the "
                f"knee-local hinge {fmt(hinge)}; the kernel rejects below "
                f"{HINGE_REJECT}")
            status = "REJECT"
        elif alignment < HINGE_WARN:
            warnings.append(
                f"{name}: mid_axis_local alignment {alignment:.3f} clears "
                f"{HINGE_REJECT} but only just; the knee-local hinge is "
                f"{fmt(hinge)}")
            status = "warn"

        if alignment >= HINGE_REJECT and signed_alignment > 0.0:
            errors.append(
                f"{name}: mid_axis_local points along the bind bend normal "
                f"{fmt(hinge)}, so the IK folds this knee backwards and the "
                f"pole twists the limb 180 degrees; negate it for this leg")
            status = "TWIST"

        if extension > REST_EXTENSION_WARN:
            warnings.append(
                f"{name}: bind pose rests at {100 * extension:.1f}% of leg "
                f"length, leaving {reach - rest:.2f} m before the IK clamps; "
                f"consider stance_crouch_meters")
            status = "warn" if status == "ok" else status

        if pole_alignment > POLE_COLLINEAR_WARN:
            warnings.append(
                f"{name}: pole_local is {pole_alignment:.3f} collinear with "
                f"the hip->foot axis and cannot pick a bend side reliably")
            status = "warn" if status == "ok" else status

        print(f"  {name:<8} reach={reach:6.2f} rest={100 * extension:5.1f}% "
              f"hinge_align={alignment:.3f} pole_axis={pole_alignment:.3f}  "
              f"{status}")

    for warning in warnings:
        print(f"  WARN  {warning}")
    for error in errors:
        print(f"  ERROR {error}")
    return 1 if errors else 0


def fmt(v):
    return "(" + ", ".join(f"{x:+.3f}" for x in v) + ")"


if __name__ == "__main__":
    sys.exit(main())
