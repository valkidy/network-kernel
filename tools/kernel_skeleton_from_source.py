#!/usr/bin/env python3
"""Generates a rig's kernel skeleton GLB and rig.yaml from its client source GLB.

A skinned actor has ONE authored source: the client GLB, a standard
(right-handed) glTF exported from the Blender master, holding the mesh, the
mixamo rig with its bone names untouched, and a GEO_* box under each bone that
carries a collider. Unity draws that file as it is. This derives the kernel's
copy from it, so the two cannot drift:

  1. Roles. The mixamo-biped preset finds the root (the armature node above
     Hips), the body (Hips) and each leg's hip, knee and foot by name, with any
     importer namespace ("mixamorig:") ignored. --root/--body/--leg override it.
  2. Bones. Only the root, the body, the leg chains and the bones that carry a
     GEO_* box are kept, together with every ancestor of them, so the subset is
     closed under parent as KernelSkeletonBinding requires. Mesh, skin and
     every other bone are dropped: the kernel overwrites whatever bones it
     keeps on every frame, and the rest belong to the client's Animator.
  3. Colliders. A GEO_* node's box is the bounds of its mesh, baked into the
     node's scale (full extents) and translation, with the mesh replaced by a
     unit cube -- the convention the rig loader reads. A GEO_* node with no
     mesh keeps its scale as the extents.
  4. Frame. The result is converted to Unity's left-handed values with the
     same conversion glTFast applies (see gltf_to_unity_handedness.py), so the
     kernel writes exactly the values the client's import already holds.
  5. Rest pose. The locomotion validator needs each knee to rest bent about
     its hinge, and a mixamo export rests with nearly straight legs. Each knee
     is bent --knee-bend degrees forward (+Z) and its hip turned back by
     whatever keeps the foot under it. Only rotations change; translations,
     and therefore bone lengths, stay identical to the client's, which is what
     keeps the skin from stretching under the kernel pose.
  6. rig.yaml. The knee hinge is computed from the rest pose on the side that
     OPENS the knee (IKTwoBoneJob opens it for a positive rotation about
     mid_axis; the other sign folds the leg backwards and twists it 180
     degrees), and each pole is the rest knee's offset from the hip-foot line.

    python3 tools/kernel_skeleton_from_source.py \\
        --source=../unity-network-example/Assets/Presentation/ActorRigs/simplified_gingerbread_giant.glb \\
        --skeleton=simplified_gingerbread_giant \\
        --glb-output=game_server/gameplay_catalog/skeleton_assets/raw/simplified_gingerbread_giant.glb \\
        --rig-output=game_server/gameplay_catalog/skeleton_assets/raw/simplified_gingerbread_giant.rig.yaml

--check regenerates in memory and compares with the two existing outputs
instead of writing them, exiting 1 on any difference: run it after changing
the source, or to confirm the committed files are what the source produces.
"""

import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gltf_to_unity_handedness as handedness  # noqa: E402

MIXAMO_LEGS = (("leg0", "Left"), ("leg1", "Right"))
COLLIDER_PREFIX = "GEO_"
# The kernel rejects a hinge below 0.5 alignment with the rest bend; this
# leaves room for authoring drift. Matches skeleton_rig_check's warning.
MIN_HINGE_ALIGNMENT = 0.70
# A generated GLB is compared node by node with this much slack, since the
# JSON round trip is the only thing that should differ.
CHECK_TOLERANCE = 1e-5


# --- small vector / quaternion helpers (no third-party dependencies) -------

def v_add(a, b):
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def v_sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def v_scale(a, s):
    return [a[0] * s, a[1] * s, a[2] * s]


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a, b):
    return [a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def v_len(a):
    return math.sqrt(v_dot(a, a))


def v_unit(a):
    length = v_len(a)
    if length <= 1e-12:
        raise ValueError("cannot normalize a zero vector")
    return v_scale(a, 1.0 / length)


def q_mul(a, b):
    return handedness.quat_multiply(a, b)


def q_conj(q):
    return [-q[0], -q[1], -q[2], q[3]]


def q_rotate(q, v):
    p = q_mul(q_mul(q, [v[0], v[1], v[2], 0.0]), q_conj(q))
    return [p[0], p[1], p[2]]


def q_axis_angle(axis, radians):
    axis = v_unit(axis)
    half = radians * 0.5
    s = math.sin(half)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half)]


def q_normalize(q):
    length = math.sqrt(sum(c * c for c in q))
    return [c / length for c in q]


# --- node graph -------------------------------------------------------------

def strip_namespace(name):
    for separator in (":", "|"):
        if separator in name:
            name = name.rsplit(separator, 1)[1]
    return name


class Graph:
    def __init__(self, gltf):
        self.nodes = gltf.get("nodes", [])
        self.parent = {}
        for index, node in enumerate(self.nodes):
            for child in node.get("children", []):
                self.parent[child] = index

    def name(self, index):
        return self.nodes[index].get("name", "node%d" % index)

    def find(self, wanted):
        """Index of the node named `wanted`, namespace-insensitive."""
        exact = [i for i in range(len(self.nodes)) if self.name(i) == wanted]
        if exact:
            return exact[0]
        loose = [i for i in range(len(self.nodes))
                 if strip_namespace(self.name(i)) == strip_namespace(wanted)]
        if len(loose) > 1:
            raise ValueError("more than one node matches '%s'" % wanted)
        return loose[0] if loose else None

    def ancestors(self, index):
        chain = []
        while index in self.parent:
            index = self.parent[index]
            chain.append(index)
        return chain

    def is_under(self, index, root):
        return index == root or root in self.ancestors(index)


def resolve_roles(graph, args):
    def need(name, what):
        index = graph.find(name)
        if index is None:
            raise ValueError("no node named '%s' for %s" % (name, what))
        return index

    body = need(args.body or "Hips", "the body bone")
    if args.root:
        root = need(args.root, "the root bone")
    else:
        if body not in graph.parent:
            raise ValueError(
                "'%s' has no parent to serve as the root bone. Keep the "
                "armature node above Hips in the export (Blender writes it as "
                "'Armature') with its transforms applied, or pass --root."
                % graph.name(body))
        root = graph.parent[body]
    legs = []
    if args.leg:
        for text in args.leg:
            leg_id, bones = text.split(":", 1)
            hip, knee, foot = bones.split(",")
            legs.append((leg_id, need(hip, leg_id), need(knee, leg_id),
                         need(foot, leg_id)))
    else:
        for leg_id, side in MIXAMO_LEGS:
            legs.append((leg_id,
                         need(side + "UpLeg", leg_id),
                         need(side + "Leg", leg_id),
                         need(side + "Foot", leg_id)))
    for leg_id, hip, knee, foot in legs:
        if graph.parent.get(knee) != hip or graph.parent.get(foot) != knee:
            raise ValueError(
                "%s: expected %s > %s > %s as direct parent and children"
                % (leg_id, graph.name(hip), graph.name(knee), graph.name(foot)))
        if not graph.is_under(hip, body):
            raise ValueError("%s: hip is not under the body bone" % leg_id)
    if not graph.is_under(body, root):
        raise ValueError("the body bone is not under the root bone")
    return root, body, legs


# --- mesh bounds and the generated unit cube ---------------------------------

def mesh_bounds(gltf, binary, mesh_index):
    low = [math.inf] * 3
    high = [-math.inf] * 3
    for primitive in gltf["meshes"][mesh_index]["primitives"]:
        accessor = gltf["accessors"][primitive["attributes"]["POSITION"]]
        if "min" in accessor and "max" in accessor:
            mins, maxs = accessor["min"], accessor["max"]
        else:
            view = handedness.Accessor(gltf, binary,
                                       primitive["attributes"]["POSITION"])
            points = [[view.get(e, c) for c in range(3)]
                      for e in range(view.count)]
            mins = [min(p[c] for p in points) for c in range(3)]
            maxs = [max(p[c] for p in points) for c in range(3)]
        low = [min(low[c], mins[c]) for c in range(3)]
        high = [max(high[c], maxs[c]) for c in range(3)]
    return low, high


def unit_cube():
    """24 vertices with per-face normals, 36 indices, outward winding."""
    faces = [
        ([1, 0, 0], [0, 1, 0], [0, 0, 1]),
        ([-1, 0, 0], [0, 0, 1], [0, 1, 0]),
        ([0, 1, 0], [0, 0, 1], [1, 0, 0]),
        ([0, -1, 0], [1, 0, 0], [0, 0, 1]),
        ([0, 0, 1], [1, 0, 0], [0, 1, 0]),
        ([0, 0, -1], [0, 1, 0], [1, 0, 0]),
    ]
    positions, normals, indices = [], [], []
    for normal, u, v in faces:
        base = len(positions)
        centre = v_scale(normal, 0.5)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            positions.append(v_add(centre, v_add(v_scale(u, 0.5 * su),
                                                 v_scale(v, 0.5 * sv))))
            normals.append(list(normal))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, indices


def cube_gltf_parts():
    positions, normals, indices = unit_cube()
    binary = bytearray()
    binary += struct.pack("<%df" % (3 * len(positions)),
                          *[c for p in positions for c in p])
    normal_offset = len(binary)
    binary += struct.pack("<%df" % (3 * len(normals)),
                          *[c for n in normals for c in n])
    index_offset = len(binary)
    binary += struct.pack("<%dH" % len(indices), *indices)
    binary += b"\0" * (-len(binary) % 4)
    buffer_views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": normal_offset,
         "target": 34962},
        {"buffer": 0, "byteOffset": normal_offset,
         "byteLength": index_offset - normal_offset, "target": 34962},
        {"buffer": 0, "byteOffset": index_offset,
         "byteLength": 2 * len(indices), "target": 34963},
    ]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(positions),
         "type": "VEC3", "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
        {"bufferView": 1, "componentType": 5126, "count": len(normals),
         "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": len(indices),
         "type": "SCALAR"},
    ]
    mesh = {"name": "ColliderUnitCube", "primitives": [
        {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}
    return binary, buffer_views, accessors, mesh


# --- generation ---------------------------------------------------------------

def trs(node):
    return (list(node.get("translation", [0.0, 0.0, 0.0])),
            list(node.get("rotation", [0.0, 0.0, 0.0, 1.0])),
            list(node.get("scale", [1.0, 1.0, 1.0])))


def build_kernel_gltf(source, binary, roles, graph):
    root, body, legs = roles
    colliders = [i for i in range(len(graph.nodes))
                 if strip_namespace(graph.name(i)).startswith(COLLIDER_PREFIX)
                 and graph.is_under(i, root)]
    if not colliders:
        raise ValueError("no %s* collider nodes under the root bone"
                         % COLLIDER_PREFIX)
    wanted = {root, body}
    for _, hip, knee, foot in legs:
        wanted.update((hip, knee, foot))
    wanted.update(colliders)
    keep = set()
    for index in wanted:
        keep.add(index)
        for ancestor in graph.ancestors(index):
            if ancestor == root:
                break
            keep.add(ancestor)
    for index in colliders:
        kept_children = [c for c in graph.nodes[index].get("children", [])
                         if c in keep]
        if kept_children:
            raise ValueError("collider '%s' has kept children"
                             % graph.name(index))

    order = []

    def visit(index):
        order.append(index)
        for child in graph.nodes[index].get("children", []):
            if child in keep:
                visit(child)

    visit(root)
    new_index = {old: new for new, old in enumerate(order)}

    cube_binary, buffer_views, accessors, cube_mesh = cube_gltf_parts()
    nodes = []
    for old in order:
        source_node = graph.nodes[old]
        translation, rotation, scale = trs(source_node)
        if old in colliders and "mesh" in source_node:
            low, high = mesh_bounds(source, binary, source_node["mesh"])
            extent = [high[c] - low[c] for c in range(3)]
            centre = [(high[c] + low[c]) * 0.5 for c in range(3)]
            offset = q_rotate(rotation, [scale[c] * centre[c] for c in range(3)])
            translation = v_add(translation, offset)
            scale = [scale[c] * extent[c] for c in range(3)]
        node = {"name": graph.name(old)}
        if any(abs(c) > 0.0 for c in translation):
            node["translation"] = translation
        if rotation != [0.0, 0.0, 0.0, 1.0]:
            node["rotation"] = rotation
        if scale != [1.0, 1.0, 1.0]:
            node["scale"] = scale
        if old in colliders:
            node["mesh"] = 0
        children = [new_index[c] for c in source_node.get("children", [])
                    if c in keep]
        if children:
            node["children"] = children
        nodes.append(node)

    gltf = {
        "asset": {"version": "2.0",
                  "generator": "kernel_skeleton_from_source.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": nodes,
        "meshes": [cube_mesh],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(cube_binary)}],
    }
    names = {
        "root": graph.name(root),
        "body": graph.name(body),
        "legs": [(leg_id, graph.name(h), graph.name(k), graph.name(f))
                 for leg_id, h, k, f in legs],
        "colliders": [graph.name(i) for i in order if i in colliders],
    }
    return gltf, cube_binary, names


class Pose:
    """Model-space rest pose of a node list (relative to node 0)."""

    def __init__(self, gltf):
        self.nodes = gltf["nodes"]
        self.parent = {}
        for index, node in enumerate(self.nodes):
            for child in node.get("children", []):
                self.parent[child] = index
        self.by_name = {n["name"]: i for i, n in enumerate(self.nodes)}
        self.position = {}
        self.rotation = {}
        self.refresh()

    def refresh(self):
        for index in range(len(self.nodes)):
            self.position.pop(index, None)
            self.rotation.pop(index, None)
        for index in range(len(self.nodes)):
            self._solve(index)

    def _solve(self, index):
        if index in self.position:
            return
        translation, rotation, _ = trs(self.nodes[index])
        if index not in self.parent:
            # The kernel's model space includes the root bone's rest transform.
            self.position[index] = translation
            self.rotation[index] = q_normalize(rotation)
            return
        parent = self.parent[index]
        self._solve(parent)
        parent_scale = trs(self.nodes[parent])[2]
        scaled = [translation[c] * parent_scale[c] for c in range(3)]
        self.position[index] = v_add(
            self.position[parent], q_rotate(self.rotation[parent], scaled))
        self.rotation[index] = q_normalize(q_mul(self.rotation[parent], rotation))

    def at(self, name):
        index = self.by_name[name]
        return self.position[index], self.rotation[index]


def leg_geometry(pose, hip, knee, foot):
    hip_p, _ = pose.at(hip)
    knee_p, knee_q = pose.at(knee)
    foot_p, _ = pose.at(foot)
    upper, lower = v_sub(knee_p, hip_p), v_sub(foot_p, knee_p)
    line = v_sub(foot_p, hip_p)
    along = v_dot(v_sub(knee_p, hip_p), line) / v_dot(line, line)
    offset = v_sub(v_sub(knee_p, hip_p), v_scale(line, along))
    bend_normal = v_cross(upper, lower)
    return {
        "hip": hip_p, "knee": knee_p, "foot": foot_p, "knee_q": knee_q,
        "offset": offset, "bend_normal": bend_normal,
        "extension": v_len(line) / (v_len(upper) + v_len(lower)),
    }


def bend_knees(gltf, names, degrees):
    """Bends each knee forward and turns its hip back to keep the foot put."""
    if degrees == 0.0:
        return []
    edits = []
    pose = Pose(gltf)
    for leg_id, hip, knee, foot in names["legs"]:
        geometry = leg_geometry(pose, hip, knee, foot)
        line = v_sub(geometry["foot"], geometry["hip"])
        # Rotating the shin about cross(forward, line) swings the foot back
        # and so pushes the knee forward.
        axis_model = v_unit(v_cross([0.0, 0.0, 1.0], line))
        knee_node = gltf["nodes"][pose.by_name[knee]]
        hip_node = gltf["nodes"][pose.by_name[hip]]
        _, knee_model_q = pose.at(knee)
        axis_knee = q_rotate(q_conj(knee_model_q), axis_model)
        knee_node["rotation"] = q_normalize(q_mul(
            trs(knee_node)[1], q_axis_angle(axis_knee, math.radians(degrees))))
        pose.refresh()
        moved = leg_geometry(pose, hip, knee, foot)
        before = v_sub(geometry["foot"], geometry["hip"])
        after = v_sub(moved["foot"], moved["hip"])
        # Signed angle from `after` back to `before` about axis_model.
        before_p = v_sub(before, v_scale(axis_model, v_dot(before, axis_model)))
        after_p = v_sub(after, v_scale(axis_model, v_dot(after, axis_model)))
        angle = math.atan2(v_dot(axis_model, v_cross(after_p, before_p)),
                           v_dot(after_p, before_p))
        _, hip_model_q = pose.at(hip)
        axis_hip = q_rotate(q_conj(hip_model_q), axis_model)
        hip_node["rotation"] = q_normalize(q_mul(
            trs(hip_node)[1], q_axis_angle(axis_hip, angle)))
        pose.refresh()
        edits.append("%s: knee %+.2f deg, hip %+.2f deg about model %s"
                     % (leg_id, degrees, math.degrees(angle),
                        ",".join("%.4f" % c for c in axis_model)))
    return edits


def solve_rig(gltf, names):
    pose = Pose(gltf)
    hinges, poles, report = [], [], []
    for leg_id, hip, knee, foot in names["legs"]:
        geometry = leg_geometry(pose, hip, knee, foot)
        if v_len(geometry["bend_normal"]) <= 1e-9:
            raise ValueError("%s: the rest leg is straight; raise --knee-bend"
                             % leg_id)
        normal_knee = v_unit(q_rotate(q_conj(geometry["knee_q"]),
                                      geometry["bend_normal"]))
        # Opposite the bend normal: a positive rotation about it opens the knee.
        hinges.append(v_scale(normal_knee, -1.0))
        poles.append(v_unit(geometry["offset"]))
        report.append((leg_id, geometry["extension"]))
    shared = v_unit([sum(h[c] for h in hinges) for c in range(3)])
    lines = []
    for (leg_id, extension), hinge in zip(report, hinges):
        alignment = v_dot(shared, hinge)
        if alignment < MIN_HINGE_ALIGNMENT:
            raise ValueError(
                "%s: the shared knee hinge is only %.3f aligned with this "
                "leg's rest bend; the legs do not bend alike" % (leg_id, alignment))
        lines.append("%s rest %.1f%% hinge %.3f" % (leg_id, 100 * extension,
                                                     alignment))
    return shared, poles, lines


def fmt_vec(v):
    return "{x: %.6f, y: %.6f, z: %.6f}" % (v[0] + 0.0, v[1] + 0.0, v[2] + 0.0)


def rig_yaml(skeleton, names, hinge, poles, command):
    leg_of = {}
    for leg_id, hip, knee, foot in names["legs"]:
        leg_of[hip] = leg_id
        leg_of[knee] = leg_id
        leg_of[foot] = leg_id
    out = [
        "# GENERATED by tools/kernel_skeleton_from_source.py -- do not edit by hand.",
        "# Regenerate from the client source GLB instead:",
        "#   " + command,
        "# GEO_* box dimensions are stored in the GLB node scales; gait timing",
        "# and other locomotion tuning belong to the entity template.",
        "rig_version: 2",
        "skeleton: " + skeleton,
        "forward_axis: positive_z",
        "root_bone: " + names["root"],
        "body_bone: " + names["body"],
        "# Knee-local, on the side that OPENS the knee: IKTwoBoneJob opens it",
        "# for a positive rotation about mid_axis.",
        "knee_hinge_local: " + fmt_vec(hinge),
        "# Model-space unit poles: each rest knee's offset from its hip-foot line.",
        "legs:",
    ]
    for (leg_id, hip, knee, foot), pole in zip(names["legs"], poles):
        out.append("  - {id: %s, hip: %s, knee: %s, foot: %s," %
                   (leg_id, hip, knee, foot))
        out.append("     pole_local: %s}" % fmt_vec(pole))
    out.append("colliders:")
    return out, leg_of


def collider_lines(gltf, names, leg_of):
    pose_parent = {}
    for index, node in enumerate(gltf["nodes"]):
        for child in node.get("children", []):
            pose_parent[child] = index
    by_name = {n["name"]: i for i, n in enumerate(gltf["nodes"])}
    lines = []
    for collider in names["colliders"]:
        index = by_name[collider]
        leg = None
        while index in pose_parent:
            index = pose_parent[index]
            leg = leg_of.get(gltf["nodes"][index]["name"])
            if leg:
                break
        if leg:
            lines.append("  - {bone: %s, leg: %s}" % (collider, leg))
        else:
            lines.append("  - {bone: %s}" % collider)
    return lines


def generate(args):
    source, binary = handedness.read_glb(args.source)
    extras = source.get("asset", {}).get("extras", {})
    if extras.get("handedness") == handedness.UNITY_HANDEDNESS:
        raise ValueError(
            args.source + " already declares " + handedness.UNITY_HANDEDNESS +
            "; the source must be the standard glTF export")
    graph = Graph(source)
    roles = resolve_roles(graph, args)
    gltf, cube_binary, names = build_kernel_gltf(source, binary, roles, graph)
    cube_binary = bytearray(cube_binary)
    handedness.convert(gltf, cube_binary, [])
    edits = bend_knees(gltf, names, args.knee_bend)
    hinge, poles, report = solve_rig(gltf, names)
    gltf["asset"]["extras"] = {
        "units": "meters",
        "metersPerUnit": 1.0,
        "upAxis": "+Y",
        "forwardAxis": "+Z",
        "transformConvention": "parent-local",
        "handedness": handedness.UNITY_HANDEDNESS,
        "conversionApplied": "x mirrored from a right-handed glTF source",
        "source": os.path.basename(args.source),
        "restPoseEdits": edits,
    }
    command = ("python3 tools/kernel_skeleton_from_source.py --source=<client GLB> "
               "--skeleton=%s --knee-bend=%g ..." % (args.skeleton, args.knee_bend))
    lines, leg_of = rig_yaml(args.skeleton, names, hinge, poles, command)
    lines += collider_lines(gltf, names, leg_of)
    return gltf, cube_binary, "\n".join(lines) + "\n", names, edits, report


def compare_glb(path, gltf):
    existing, _ = handedness.read_glb(path)
    a, b = existing.get("nodes", []), gltf["nodes"]
    problems = []
    if [n.get("name") for n in a] != [n.get("name") for n in b]:
        problems.append("node names or order differ")
        return problems
    for left, right in zip(a, b):
        if left.get("children", []) != right.get("children", []):
            problems.append(left["name"] + ": children differ")
        for key, default in (("translation", [0, 0, 0]),
                             ("rotation", [0, 0, 0, 1]),
                             ("scale", [1, 1, 1])):
            l, r = left.get(key, default), right.get(key, default)
            if key == "rotation" and sum(x * y for x, y in zip(l, r)) < 0:
                r = [-x for x in r]
            if max(abs(x - y) for x, y in zip(l, r)) > CHECK_TOLERANCE:
                problems.append("%s: %s differs" % (left["name"], key))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", required=True)
    parser.add_argument("--skeleton", required=True)
    parser.add_argument("--glb-output", required=True)
    parser.add_argument("--rig-output", required=True)
    parser.add_argument("--knee-bend", type=float, default=30.0,
                        help="degrees each rest knee is bent forward (0 keeps "
                             "the source rest pose)")
    parser.add_argument("--root", help="root bone name (default: Hips' parent)")
    parser.add_argument("--body", help="body bone name (default: Hips)")
    parser.add_argument("--leg", action="append",
                        help="ID:HIP,KNEE,FOOT; repeatable (default: mixamo "
                             "LeftUpLeg/LeftLeg/LeftFoot as leg0, Right as leg1)")
    parser.add_argument("--check", action="store_true",
                        help="compare with the existing outputs instead of "
                             "writing them")
    args = parser.parse_args()

    try:
        gltf, binary, rig_text, names, edits, report = generate(args)
    except ValueError as error:
        print("error: %s" % error, file=sys.stderr)
        return 2

    print("%s: %d bones kept (%s ... ), %d colliders"
          % (args.skeleton, len(gltf["nodes"]), names["root"],
             len(names["colliders"])))
    for line in edits + report:
        print("  " + line)

    if args.check:
        problems = []
        if not os.path.exists(args.glb_output):
            problems.append(args.glb_output + " does not exist")
        else:
            problems += compare_glb(args.glb_output, gltf)
        if not os.path.exists(args.rig_output):
            problems.append(args.rig_output + " does not exist")
        elif open(args.rig_output, encoding="utf-8").read() != rig_text:
            problems.append(args.rig_output + " differs from the generated text")
        for problem in problems:
            print("  DRIFT " + problem)
        print("  up to date" if not problems else "  out of date")
        return 1 if problems else 0

    handedness.write_glb(args.glb_output, gltf, binary)
    with open(args.rig_output, "w", encoding="utf-8") as stream:
        stream.write(rig_text)
    print("  wrote " + args.glb_output)
    print("  wrote " + args.rig_output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
