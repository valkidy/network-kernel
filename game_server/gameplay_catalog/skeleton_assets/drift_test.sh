#!/usr/bin/env bash
# Fails when a rig's checked-in asset no longer matches what its GLB converts
# to on this machine: the GLB changed and the checked-in files were not
# regenerated. Arguments come in (checked-in, regenerated) pairs.
set -euo pipefail

status=0
while [ $# -ge 2 ]; do
    if ! cmp -s "$1" "$2"; then
        echo "drift: $1 differs from what its GLB converts to ($2)." >&2
        status=1
    fi
    shift 2
done
if [ "$status" -ne 0 ]; then
    echo "Run tools/regenerate_skeleton_assets.sh on macOS and commit checked_in/." >&2
fi
exit "$status"
