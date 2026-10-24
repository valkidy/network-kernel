def skeleton_asset(name, src, asset_id, rig = None):
    """Ships one rig's checked-in Ozz asset and manifest, and checks them for drift.

    The runtime asset and its manifest are checked in under checked_in/ and only
    copied at build time, not converted from the GLB. gltf2ozz does floating
    point while it decomposes the bone transforms, and arm64/clang and
    x86_64/gcc round a bone differently in the last bit. The manifest's
    content_hash covers the .ozz bytes, so a Linux build hashed every rig
    differently from a macOS one, and a client whose prefab pins the macOS hash
    rejected every pose a Linux server's bundle described: the gingerbread giant
    stood in its rest pose, sunk into the terrain. Checked-in bytes are the same
    bytes on every platform.

    <name>_from_glb still converts the GLB, into regenerated/. On macOS,
    <name>_drift_test fails when that differs from checked_in/ -- the GLB
    changed and tools/regenerate_skeleton_assets.sh was not run. It cannot run
    elsewhere: anywhere else the conversion is expected to differ.

    rig optionally names an authored <name>.rig.yaml describing what the rig IS
    -- today its per-bone colliders. It is copied next to the generated files
    rather than kept in raw/, so that all three of a rig's files share one
    directory and one basename and the catalog's existing skeleton_manifests_dir
    scan finds them by suffix alone. It stays hand-authored because the manifest
    beside it is generated from the GLB and cannot carry authored data.
    """
    runtime_file = "{}.ozz".format(name)
    manifest_file = "{}.skeleton_manifest.json".format(name)

    native.genrule(
        name = name + "_from_glb",
        srcs = [src],
        outs = ["regenerated/" + runtime_file, "regenerated/" + manifest_file],
        cmd = """
set -euo pipefail
$(location @ozz_animation//:gltf2ozz) \\
  --file=$(location {src}) \\
  --config='{{"skeleton":{{"filename":"$(@D)/regenerated/{runtime_file}","import":{{"enable":true}}}},"animations":[]}}'
$(location //tools:ozz_skeleton_manifest) \\
  --skeleton=$(@D)/regenerated/{runtime_file} \\
  --output=$(@D)/regenerated/{manifest_file} \\
  --asset-id={asset_id} \\
  --name={name} \\
  --runtime-file={runtime_file}
""".format(
            asset_id = asset_id,
            manifest_file = manifest_file,
            name = name,
            runtime_file = runtime_file,
            src = src,
        ),
        tools = [
            "//tools:ozz_skeleton_manifest",
            "@ozz_animation//:gltf2ozz",
        ],
    )

    native.genrule(
        name = name + "_generated",
        srcs = ["checked_in/" + runtime_file, "checked_in/" + manifest_file],
        outs = ["generated/" + runtime_file, "generated/" + manifest_file],
        cmd = "cp $(SRCS) $(RULEDIR)/generated/",
    )

    native.sh_test(
        name = name + "_drift_test",
        srcs = ["drift_test.sh"],
        args = [
            "$(rootpath checked_in/{})".format(runtime_file),
            "$(rootpath :regenerated/{})".format(runtime_file),
            "$(rootpath checked_in/{})".format(manifest_file),
            "$(rootpath :regenerated/{})".format(manifest_file),
        ],
        data = [
            "checked_in/" + runtime_file,
            "checked_in/" + manifest_file,
            ":regenerated/" + runtime_file,
            ":regenerated/" + manifest_file,
        ],
        target_compatible_with = select({
            "@//engine:macos": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )

    asset_srcs = [":" + name + "_generated"]
    if rig != None:
        rig_output = "generated/{}.rig.yaml".format(name)
        native.genrule(
            name = name + "_rig",
            srcs = [rig],
            outs = [rig_output],
            cmd = "cp $(location {rig}) $@".format(rig = rig),
        )
        asset_srcs.append(":" + name + "_rig")
    native.filegroup(
        name = name + "_assets",
        srcs = asset_srcs,
    )
