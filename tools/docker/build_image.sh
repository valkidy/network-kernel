#!/usr/bin/env bash
# Builds the dedicated server runtime image and smoke-tests it.
#
#   1. Build //app:app -c opt and the catalog bundle in the builder image
#      (same toolchain and Bazel cache volume as build_linux.sh), and stage
#      the binary, the bundle and any Bazel-built shared library it links.
#   2. Build tools/docker/Dockerfile from that staging directory, tagged
#      <git sha>[-dirty]-abi<N>-proto<N> and latest.
#   3. Smoke: start the server container, connect a client container to it
#      over the Docker network, require a PlayerJoined event in the server
#      log, then `docker stop` it and require exit code 0.
#
# Run from the repository root on an x86_64 Linux host (WSL2 included):
#   tools/docker/build_image.sh
# NK_SKIP_SMOKE=1 skips step 3.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILDER_IMAGE=network-kernel-builder:latest
CACHE_VOLUME=network-kernel-bazel-cache
IMAGE_REPO=network-kernel-server

# Tag: what the client has to match is the ABI and protocol version, so they
# are in the name rather than only in the image's build info log.
abi="$(sed -n 's/^#define KERNEL_ABI_VERSION \([0-9]*\)u$/\1/p' \
    "$REPO_ROOT/engine/src/kernel/public/kernel_types.h")"
proto="$(sed -n 's/^constexpr std::uint16_t kProtocolVersion = \([0-9]*\);$/\1/p' \
    "$REPO_ROOT/engine/src/protocol/public/packet_header.h")"
sha="$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
if [ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no)" ]; then
    sha="$sha-dirty"
fi
if [ -z "$abi" ] || [ -z "$proto" ]; then
    echo "could not read KERNEL_ABI_VERSION / kProtocolVersion" >&2
    exit 1
fi
VERSION="$sha-abi$abi-proto$proto"
IMAGE="$IMAGE_REPO:$VERSION"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "== [1/3] build and stage ($VERSION)"
docker build --platform linux/amd64 -t "$BUILDER_IMAGE" \
    -f "$REPO_ROOT/tools/docker/builder.Dockerfile" "$REPO_ROOT/tools/docker"

# --symlink_prefix=/ keeps the container from repointing the host's
# bazel-bin / bazel-out symlinks at paths that only exist in the container.
docker run --rm --platform linux/amd64 \
    -v "$REPO_ROOT:/src" \
    -v "$CACHE_VOLUME:/root/.cache/bazel" \
    -v "$STAGE:/out" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$BUILDER_IMAGE" bash -c '
set -euo pipefail
bazel build -c opt --symlink_prefix=/ \
    //app:app //game_server/gameplay_catalog_bundle:bundle.zip
BIN="$(bazel info -c opt bazel-bin)"
mkdir -p /out/bin /out/data /out/lib
cp "$BIN/app/app" /out/bin/server
strip --strip-unneeded /out/bin/server
cp "$BIN/game_server/gameplay_catalog_bundle/bundle.zip" /out/data/bundle.zip
cp /src/tools/docker/entrypoint.sh /out/entrypoint.sh

# Anything the binary loads from the Bazel output tree has to ship with it;
# system libraries come from the identical base image.
if ldd "$BIN/app/app" | grep -q "not found"; then
    ldd "$BIN/app/app" >&2
    echo "unresolved shared library" >&2
    exit 1
fi
ldd "$BIN/app/app" | awk "/=> \\// { print \$3 }" | grep -E "bazel|_solib" |
    while read -r lib; do cp -L "$lib" /out/lib/; done || true
echo "linked from the Bazel tree: $(ls /out/lib | wc -l) libraries"
chown -R "$HOST_UID:$HOST_GID" /out
'

echo "== [2/3] runtime image"
docker build --platform linux/amd64 \
    --build-arg NK_IMAGE_VERSION="$VERSION" \
    -t "$IMAGE" -t "$IMAGE_REPO:latest" \
    -f "$REPO_ROOT/tools/docker/Dockerfile" "$STAGE"
docker image ls "$IMAGE" --format '{{.Repository}}:{{.Tag}}  {{.Size}}'

if [ "${NK_SKIP_SMOKE:-0}" = "1" ]; then
    echo "built $IMAGE (smoke skipped)"
    exit 0
fi

echo "== [3/3] smoke"
SERVER=nk-smoke-server
docker rm -f "$SERVER" >/dev/null 2>&1 || true
trap 'docker rm -f "$SERVER" >/dev/null 2>&1 || true; rm -rf "$STAGE"' EXIT
docker run -d --name "$SERVER" --platform linux/amd64 "$IMAGE" >/dev/null

for _ in $(seq 1 40); do
    docker logs "$SERVER" 2>&1 | grep -q "dedicated server listening" && break
    sleep 0.5
done
if ! docker logs "$SERVER" 2>&1 | grep -q "dedicated server listening"; then
    docker logs "$SERVER" 2>&1 | tail -40
    echo "SMOKE FAIL: server never started listening" >&2
    exit 1
fi

# GNS parses IP addresses only, so connect by the container's IP. Reaching
# it from another container also proves the server binds beyond loopback.
SERVER_IP="$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SERVER")"
docker run --rm --platform linux/amd64 --entrypoint /usr/bin/timeout "$IMAGE" \
    12 /opt/nk/bin/server --mode=client --address="$SERVER_IP:7801" \
    --catalog-cache-dir=/tmp/nk-catalog-cache > "$STAGE/client.log" 2>&1 || true

if ! docker logs "$SERVER" 2>&1 | grep -q "event type=2 "; then
    docker logs "$SERVER" 2>&1 | tail -40
    echo "--- client" >&2
    tail -40 "$STAGE/client.log" >&2
    echo "SMOKE FAIL: no PlayerJoined on the server" >&2
    exit 1
fi

stop_start=$(date +%s%N)
docker stop -t 5 "$SERVER" >/dev/null
stop_ms=$(( ($(date +%s%N) - stop_start) / 1000000 ))
exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$SERVER")"
if [ "$exit_code" != "0" ] || ! docker logs "$SERVER" 2>&1 | grep -q "dedicated server stopped"; then
    docker logs "$SERVER" 2>&1 | tail -20
    echo "SMOKE FAIL: docker stop gave exit code $exit_code" >&2
    exit 1
fi

echo "SMOKE OK: client joined over the Docker network; docker stop took ${stop_ms} ms, exit 0"
echo "image: $IMAGE"
echo "run:   NK_IMAGE=$IMAGE docker compose -f tools/docker/compose.yaml up -d"
