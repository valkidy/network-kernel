#!/usr/bin/env bash
# Converts every rig's GLB and writes the result over its checked-in asset
# (game_server/gameplay_catalog/skeleton_assets/checked_in/).
#
# macOS only. The checked-in bytes are what every platform ships, and the
# Unity prefabs pin their content_hash; gltf2ozz rounds differently on Linux,
# so regenerating there would change every hash and break those prefabs.
#
# Run from the repository root:
#   tools/regenerate_skeleton_assets.sh
# then check the printed hashes against the prefabs' SkeletonContentHash and
# commit checked_in/.
set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "regenerate on macOS only: the checked-in rigs are the macOS conversion." >&2
    exit 1
fi

PACKAGE=game_server/gameplay_catalog/skeleton_assets
targets=$(bazel query "attr(name, '_from_glb\$', //$PACKAGE:*)")
bazel build $targets

BIN="$(bazel info bazel-bin)/$PACKAGE/regenerated"
for manifest in "$BIN"/*.skeleton_manifest.json; do
    name=$(basename "$manifest" .skeleton_manifest.json)
    cp "$BIN/$name.ozz" "$manifest" "$PACKAGE/checked_in/"
    chmod 644 "$PACKAGE/checked_in/$name.ozz" "$PACKAGE/checked_in/$name.skeleton_manifest.json"
    hash=$(sed -n 's/.*"content_hash": "\(0x[0-9a-f]*\)".*/\1/p' "$manifest")
    echo "$name content_hash=$hash ($(printf '%u' "$hash"))"
done
