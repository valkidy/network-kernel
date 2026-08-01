def skeleton_asset(name, src, asset_id):
    """Converts one GLB skeleton and emits its runtime Ozz asset and manifest."""
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
    native.filegroup(
        name = name + "_assets",
        srcs = [":" + name + "_generated"],
    )
