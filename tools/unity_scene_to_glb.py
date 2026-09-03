#!/usr/bin/env python3
"""Re-exports a rig from a Unity scene into a GLB, with no coordinate conversion.

The skeleton pipeline is component-identity end to end: gltf2ozz copies glTF
local TRS into ozz verbatim, and KernelSkeletonPoseApplicator copies the
kernel's bone transforms into Unity Transforms verbatim. Nothing in the chain
flips an axis. A GLB that went through a right-handed/left-handed conversion on
the way out therefore desynchronises the rig from the scene it was authored in.

This exporter writes the Unity scene's local TRS values straight into the glTF
nodes so that property holds by construction. Geometry (meshes, accessors,
buffers, materials) is carried over unchanged from a template GLB, keyed by node
name, so a re-export only ever rewrites the node graph.

The Unity scene lives in the separate unity-network-example project, so this
runs as a plain script rather than a Bazel target -- the scene is not a
checked-in input of this repo:

    python3 tools/unity_scene_to_glb.py \
        --scene=../unity-network-example/Assets/Scenes/LocomotionModel.unity \
        --rig=Tripod \
        --template=game_server/shipping_catalog/skeleton_assets/raw/simplified_tripod.glb \
        --output=game_server/shipping_catalog/skeleton_assets/raw/simplified_tripod.glb
"""

import argparse
import json
import re
import struct
import sys

# Unity built-in mesh fileIDs, for reporting only; geometry comes from the
# template GLB.
UNITY_BUILTIN_MESHES = {10202: "Cube", 10207: "Sphere", 10209: "Plane"}

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

# Declares the frame the file is authored in. glTF's own convention is -Z
# forward; these rigs are authored +Z forward in Unity and must not be
# converted, so the file says so explicitly rather than leaving readers to
# assume the spec default.
ASSET_EXTRAS = {
    "units": "meters",
    "metersPerUnit": 1.0,
    "upAxis": "+Y",
    "forwardAxis": "+Z",
    "transformConvention": "parent-local",
    "handedness": "unity-left-handed",
    "conversionApplied": "none",
}


def parse_unity_scene(path):
    """Returns (transforms, gameobjects, mesh_of_gameobject) keyed by fileID."""
    text = open(path, encoding="utf-8").read()
    blocks = re.split(r"^--- !u!(\d+) &(\d+).*$", text, flags=re.M)

    gameobjects = {}
    transforms = {}
    mesh_of = {}
    for index in range(1, len(blocks), 3):
        class_id, file_id, body = blocks[index], blocks[index + 1], blocks[index + 2]
        if class_id == "1":
            match = re.search(r"^  m_Name: (.*)$", body, re.M)
            gameobjects[file_id] = match.group(1).strip() if match else ""
        elif class_id == "4":
            def vec(key):
                match = re.search(
                    r"^  " + key + r": \{x: ([-\d.eE+]+), y: ([-\d.eE+]+), "
                    r"z: ([-\d.eE+]+)(?:, w: ([-\d.eE+]+))?\}",
                    body,
                    re.M,
                )
                if match is None:
                    return None
                return [float(g) for g in match.groups() if g is not None]

            owner = re.search(r"^  m_GameObject: \{fileID: (\d+)\}", body, re.M)
            father = re.search(r"^  m_Father: \{fileID: (\d+)\}", body, re.M)
            order = re.findall(r"^  - \{fileID: (\d+)\}$", body, re.M)
            transforms[file_id] = {
                "gameobject": owner.group(1) if owner else None,
                "father": father.group(1) if father and father.group(1) != "0" else None,
                "children": order,
                "translation": vec("m_LocalPosition"),
                "rotation": vec("m_LocalRotation"),
                "scale": vec("m_LocalScale"),
            }
        elif class_id == "33":
            owner = re.search(r"^  m_GameObject: \{fileID: (\d+)\}", body, re.M)
            mesh = re.search(r"^  m_Mesh: \{fileID: (\d+)", body, re.M)
            if owner and mesh:
                mesh_of[owner.group(1)] = int(mesh.group(1))

    for transform in transforms.values():
        transform["name"] = gameobjects.get(transform["gameobject"], "")
    return transforms, mesh_of


def read_glb(path):
    data = open(path, "rb").read()
    magic, _version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise SystemExit(f"not a GLB: {path}")
    offset = 12
    gltf = None
    binary = b""
    while offset < length:
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        payload = data[offset + 8 : offset + 8 + chunk_length]
        if chunk_type == CHUNK_JSON:
            gltf = json.loads(payload.decode("utf-8"))
        elif chunk_type == CHUNK_BIN:
            binary = payload
        offset += 8 + chunk_length
        offset += (4 - chunk_length % 4) % 4 if chunk_length % 4 else 0
    return gltf, binary


def write_glb(path, gltf, binary):
    json_chunk = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((4 - len(json_chunk) % 4) % 4)
    bin_chunk = binary + b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(json_chunk) + (8 + len(bin_chunk) if bin_chunk else 0)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", GLB_MAGIC, 2, total))
        out.write(struct.pack("<II", len(json_chunk), CHUNK_JSON))
        out.write(json_chunk)
        if bin_chunk:
            out.write(struct.pack("<II", len(bin_chunk), CHUNK_BIN))
            out.write(bin_chunk)
    return total


def export(scene_path, rig_name, template_path, output_path, root_name):
    transforms, mesh_of = parse_unity_scene(scene_path)
    roots = [f for f, t in transforms.items() if t["name"] == rig_name]
    if len(roots) != 1:
        raise SystemExit(
            f"expected exactly one '{rig_name}' transform in {scene_path}, "
            f"found {len(roots)}"
        )

    template, binary = read_glb(template_path)
    # Geometry is keyed by node name so a renamed or re-parented node keeps its
    # mesh, and a node the template did not have simply carries none.
    template_mesh_of_name = {
        node.get("name"): node["mesh"]
        for node in template.get("nodes", [])
        if "mesh" in node
    }

    nodes = []
    # Wrapper root. Without a skin, gltf2ozz treats every scene node as a joint
    # and roots the skeleton here, so the chain above SIM_Root must be identical
    # across rigs or their bone indices diverge for no reason.
    nodes.append({"name": root_name, "children": [1]})

    unmatched = []

    def emit(file_id, is_rig_root):
        transform = transforms[file_id]
        index = len(nodes)
        node = {"name": transform["name"]}
        nodes.append(node)
        if is_rig_root:
            # The rig sits wherever it was placed in the scene; the asset is
            # authored about its own origin.
            pass
        else:
            if any(abs(v) > 1e-9 for v in transform["translation"]):
                node["translation"] = [float(v) for v in transform["translation"]]
            rotation = transform["rotation"]
            if any(abs(v) > 1e-9 for v in rotation[:3]) or abs(rotation[3] - 1.0) > 1e-9:
                node["rotation"] = [float(v) for v in rotation]
            if any(abs(v - 1.0) > 1e-9 for v in transform["scale"]):
                node["scale"] = [float(v) for v in transform["scale"]]
        mesh_id = mesh_of.get(transform["gameobject"])
        if mesh_id is not None:
            mesh_index = template_mesh_of_name.get(transform["name"])
            if mesh_index is None:
                unmatched.append(
                    (transform["name"], UNITY_BUILTIN_MESHES.get(mesh_id, mesh_id))
                )
            else:
                node["mesh"] = mesh_index
        children = [c for c in transform["children"] if c in transforms]
        if children:
            node["children"] = [emit(c, False) for c in children]
        return index

    emit(roots[0], True)

    if unmatched:
        for name, kind in unmatched:
            print(f"  WARNING: {name} has a Unity {kind} mesh with no template "
                  f"geometry; exported without a mesh", file=sys.stderr)

    gltf = {
        "asset": {
            "version": "2.0",
            "generator": "unity_scene_to_glb.py",
            "extras": ASSET_EXTRAS,
        },
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": nodes,
    }
    for key in ("meshes", "accessors", "bufferViews", "buffers", "materials"):
        if key in template:
            gltf[key] = template[key]

    size = write_glb(output_path, gltf, binary)
    print(f"{rig_name}: {len(nodes)} nodes -> {output_path} ({size} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--rig", required=True)
    parser.add_argument("--template", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--root-name", default="world")
    args = parser.parse_args()
    export(args.scene, args.rig, args.template, args.output, args.root_name)


if __name__ == "__main__":
    main()
