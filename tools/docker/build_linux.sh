#!/usr/bin/env bash
# Builds the dedicated server for linux/amd64 inside the builder image and
# smoke-runs it: the server must start, print its build info, and still be
# running when the timeout stops it.
#
# Run from the repository root on an x86_64 Linux host (WSL2 included):
#   tools/docker/build_linux.sh
set -euo pipefail

IMAGE=network-kernel-builder:latest
CACHE_VOLUME=network-kernel-bazel-cache
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

docker build --platform linux/amd64 -t "$IMAGE" \
    -f "$REPO_ROOT/tools/docker/builder.Dockerfile" "$REPO_ROOT/tools/docker"

# --symlink_prefix=/ keeps the container from repointing the host's
# bazel-bin / bazel-out symlinks at paths that only exist in the container.
docker run --rm --platform linux/amd64 \
    -v "$REPO_ROOT:/src" \
    -v "$CACHE_VOLUME:/root/.cache/bazel" \
    "$IMAGE" bash -c '
set -euo pipefail
bazel build -c opt --symlink_prefix=/ \
    //app:app //game_server/gameplay_catalog_bundle:bundle.zip
BIN="$(bazel info -c opt bazel-bin)"
set +e
timeout 10 "$BIN/app/app" --mode=dedicated_server --port=7801 \
    --gameplay-catalog-bundle="$BIN/game_server/gameplay_catalog_bundle/bundle.zip" \
    --gameplay-catalog-entry=gameplay_catalog.yaml > /tmp/server.log 2>&1
status=$?
set -e
cat /tmp/server.log | head -40
if [ "$status" -ne 124 ]; then
    echo "SMOKE FAIL: server exited with $status before the timeout" >&2
    exit 1
fi
grep -q "build_platform" /tmp/server.log || { echo "SMOKE FAIL: no build info" >&2; exit 1; }
echo "SMOKE OK: server ran for 10 s on linux/amd64"
'
