#!/usr/bin/env python3
"""Converts a standard right-handed GLB into the skeleton pipeline's frame.

The skeleton pipeline stores Unity's left-handed local TRS values verbatim in
the glTF nodes (see unity_scene_to_glb.py): gltf2ozz copies them into ozz, and
KernelSkeletonPoseApplicator copies the kernel pose into Unity Transforms, with
nothing flipping an axis on the way. A GLB exported by a DCC tool (Blender,
mixamo) instead follows the glTF spec, which is right-handed. Fed to the
pipeline unconverted, such a rig is mirrored relative to the Unity scene: its
left leg is drawn on Unity's right, and a skinned mesh imported by glTFast
(which does convert) cannot follow the pose at all.

This applies the same conversion glTFast does, M = diag(-1, 1, 1), so the file
ends up holding exactly the values glTFast would show in Unity:

    node translation   (x, y, z)    -> (-x, y, z)
    node rotation      (x, y, z, w) -> (x, -y, -z, w)
    node scale         unchanged
    POSITION / NORMAL  x negated, triangle winding reversed
    TANGENT            x negated (w handedness flipped)
    inverse binds      M * IBM * M

Optionally, --rotate post-multiplies a node's converted local rotation by an
axis-angle rotation in that node's own frame. It re-authors a rest pose, for
example to give a nearly straight knee the bind-pose bend the locomotion
validator needs, without leaving the frame the rest of the file is in:

    python3 tools/gltf_to_unity_handedness.py \\
        --input=source.glb \\
        --output=game_server/gameplay_catalog/skeleton_assets/raw/rig.glb \\
        --rotate=JNT_Leg0_Knee:1,0,0:-30

A file that already declares asset.extras.handedness == "unity-left-handed"
is refused, since converting it twice would mirror it back.
"""

import argparse
import json
import math
import struct
import sys

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

COMPONENT_FORMATS = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}
TYPE_COUNTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
UNITY_HANDEDNESS = "unity-left-handed"


def read_glb(path):
    with open(path, "rb") as stream:
        data = stream.read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2:
        raise ValueError(path + " is not a glTF 2.0 binary")
    offset = 12
    gltf = None
    binary = b""
    while offset < len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8:offset + 8 + length]
        if kind == CHUNK_JSON:
            gltf = json.loads(chunk)
        elif kind == CHUNK_BIN:
            binary = chunk
        offset += 8 + length
    if gltf is None:
        raise ValueError(path + " has no JSON chunk")
    return gltf, bytearray(binary)


def write_glb(path, gltf, binary):
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    binary = bytes(binary) + b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(json_bytes) + (8 + len(binary) if binary else 0)
    with open(path, "wb") as stream:
        stream.write(struct.pack("<III", GLB_MAGIC, 2, total))
        stream.write(struct.pack("<II", len(json_bytes), CHUNK_JSON))
        stream.write(json_bytes)
        if binary:
            stream.write(struct.pack("<II", len(binary), CHUNK_BIN))
            stream.write(binary)


class Accessor:
    """Element-wise view of one accessor inside the binary chunk."""

    def __init__(self, gltf, binary, index):
        accessor = gltf["accessors"][index]
        if "sparse" in accessor:
            raise ValueError("sparse accessor %d is not supported" % index)
        view = gltf["bufferViews"][accessor["bufferView"]]
        self.accessor = accessor
        self.binary = binary
        self.format = "<" + COMPONENT_FORMATS[accessor["componentType"]]
        self.size = struct.calcsize(self.format)
        self.width = TYPE_COUNTS[accessor["type"]]
        self.count = accessor["count"]
        self.stride = view.get("byteStride", self.size * self.width)
        self.base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)

    def offset(self, element, component):
        return self.base + element * self.stride + component * self.size

    def get(self, element, component):
        return struct.unpack_from(
            self.format, self.binary, self.offset(element, component))[0]

    def set(self, element, component, value):
        struct.pack_into(
            self.format, self.binary, self.offset(element, component), value)


def negate_x(gltf, binary, index, flip_w=False):
    view = Accessor(gltf, binary, index)
    for element in range(view.count):
        view.set(element, 0, -view.get(element, 0))
        if flip_w:
            view.set(element, 3, -view.get(element, 3))
    accessor = view.accessor
    if "min" in accessor and "max" in accessor:
        low, high = accessor["min"][0], accessor["max"][0]
        accessor["min"][0], accessor["max"][0] = -high, -low


def reverse_winding(gltf, binary, index):
    view = Accessor(gltf, binary, index)
    for triangle in range(view.count // 3):
        first = triangle * 3
        second = view.get(first + 1, 0)
        view.set(first + 1, 0, view.get(first + 2, 0))
        view.set(first + 2, 0, second)


def mirror_matrix(gltf, binary, index):
    # Column-major 4x4; M * A * M negates every element whose row or column
    # (but not both) is the X axis.
    view = Accessor(gltf, binary, index)
    for element in range(view.count):
        for column in range(4):
            for row in range(4):
                if (row == 0) != (column == 0):
                    component = column * 4 + row
                    view.set(element, component, -view.get(element, component))


def quat_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]


def axis_angle(axis, degrees):
    length = math.sqrt(sum(component * component for component in axis))
    if length <= 0.0:
        raise ValueError("rotation axis must be non-zero")
    half = math.radians(degrees) * 0.5
    scale = math.sin(half) / length
    return [axis[0] * scale, axis[1] * scale, axis[2] * scale, math.cos(half)]


def parse_rotate(text):
    try:
        node, axis, degrees = text.split(":")
        components = [float(value) for value in axis.split(",")]
        if len(components) != 3:
            raise ValueError
        return node, components, float(degrees)
    except ValueError:
        raise argparse.ArgumentTypeError(
            "expected NODE:AX,AY,AZ:DEGREES, got '" + text + "'")


def convert(gltf, binary, rotations):
    for node in gltf.get("nodes", []):
        if "matrix" in node:
            raise ValueError(
                "node '" + node.get("name", "?") + "' uses a matrix; only TRS "
                "nodes are supported")
        if "translation" in node:
            node["translation"][0] = -node["translation"][0]
        if "rotation" in node:
            x, y, z, w = node["rotation"]
            node["rotation"] = [x, -y, -z, w]

    mirrored = set()
    for mesh in gltf.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            mode = primitive.get("mode", 4)
            if mode != 4:
                raise ValueError("only triangle-list primitives are supported")
            attributes = primitive["attributes"]
            targets = [attributes] + primitive.get("targets", [])
            for attribute_set in targets:
                for name in ("POSITION", "NORMAL"):
                    index = attribute_set.get(name)
                    if index is not None and index not in mirrored:
                        negate_x(gltf, binary, index)
                        mirrored.add(index)
                tangent = attribute_set.get("TANGENT")
                if tangent is not None and tangent not in mirrored:
                    negate_x(gltf, binary, tangent, flip_w=True)
                    mirrored.add(tangent)
            if "indices" not in primitive:
                raise ValueError("non-indexed primitives are not supported")
            if primitive["indices"] not in mirrored:
                reverse_winding(gltf, binary, primitive["indices"])
                mirrored.add(primitive["indices"])

    for skin in gltf.get("skins", []):
        index = skin.get("inverseBindMatrices")
        if index is not None and index not in mirrored:
            mirror_matrix(gltf, binary, index)
            mirrored.add(index)

    by_name = {node.get("name"): node for node in gltf.get("nodes", [])}
    for name, axis, degrees in rotations:
        node = by_name.get(name)
        if node is None:
            raise ValueError("--rotate names unknown node '" + name + "'")
        node["rotation"] = quat_multiply(
            node.get("rotation", [0.0, 0.0, 0.0, 1.0]),
            axis_angle(axis, degrees))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--rotate", type=parse_rotate, action="append", default=[],
        help="NODE:AX,AY,AZ:DEGREES, post-multiplied in the node's local "
             "frame after conversion; repeatable")
    args = parser.parse_args()

    gltf, binary = read_glb(args.input)
    asset = gltf.setdefault("asset", {"version": "2.0"})
    extras = asset.setdefault("extras", {})
    if extras.get("handedness") == UNITY_HANDEDNESS:
        sys.exit(args.input + " already declares " + UNITY_HANDEDNESS)

    convert(gltf, binary, args.rotate)
    extras.update({
        "units": "meters",
        "metersPerUnit": 1.0,
        "upAxis": "+Y",
        "forwardAxis": "+Z",
        "transformConvention": "parent-local",
        "handedness": UNITY_HANDEDNESS,
        "conversionApplied": "x mirrored from a right-handed glTF source",
    })
    if args.rotate:
        extras["restPoseEdits"] = [
            "%s:%s:%g" % (name, ",".join("%g" % c for c in axis), degrees)
            for name, axis, degrees in args.rotate
        ]
    write_glb(args.output, gltf, binary)
    print("wrote " + args.output)


if __name__ == "__main__":
    main()
