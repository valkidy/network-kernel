#!/bin/sh
# Turns NK_* environment variables into dedicated server flags and execs the
# server, so it runs as PID 1 and receives `docker stop`'s SIGTERM itself.
# Extra container arguments are passed through, e.g. --network-stats=basic.
set -eu

: "${NK_PORT:=7801}"
: "${NK_MAX_PLAYERS:=4}"
: "${NK_NETCODE:=standard}"

exec /opt/nk/bin/server \
    --mode=dedicated_server \
    --port="$NK_PORT" \
    --max-players="$NK_MAX_PLAYERS" \
    --netcode="$NK_NETCODE" \
    --gameplay-catalog-bundle=/opt/nk/data/bundle.zip \
    --gameplay-catalog-entry=gameplay_catalog.yaml \
    "$@"
