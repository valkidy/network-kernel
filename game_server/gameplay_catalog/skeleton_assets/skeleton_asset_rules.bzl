def skeleton_asset(name, src, asset_id, rig = None):
    """Converts one GLB skeleton and emits its runtime Ozz asset and manifest.

    rig optionally names an authored <name>.rig.yaml describing what the rig IS
    -- today its per-bone colliders. It is copied next to the generated files
    rather than kept in raw/, so that all three of a rig's files share one
    directory and one basename and the catalog's existing skeleton_manifests_dir
    scan finds them by suffix alone. It stays hand-authored because the manifest
    beside it is generated from the GLB and cannot carry authored data.
    """
    runtime_output = "generated/{}.ozz".format(name)
    manifest_output = "generated/{}.skeleton_manifest.json".format(name)
    native.genrule(
        name = name + "_generated",
        srcs = [src],
        outs = [runtime_output, manifest_output],
        cmd = """
set -euo pipefail
$(location @ozz_animation//:gltf2ozz) \\
  --file=$(location {src}) \\
  --config='{{"skeleton":{{"filename":"$(@D)/{runtime_output}","import":{{"enable":true}}}},"animations":[]}}'
$(location //tools:ozz_skeleton_manifest) \\
  --skeleton=$(@D)/{runtime_output} \\
  --output=$(@D)/{manifest_output} \\
  --asset-id={asset_id} \\
  --name={name} \\
  --runtime-file={name}.ozz
""".format(
            asset_id = asset_id,
            manifest_output = manifest_output,
            name = name,
            runtime_output = runtime_output,
            src = src,
        ),
        tools = [
            "//tools:ozz_skeleton_manifest",
            "@ozz_animation//:gltf2ozz",
        ],
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
