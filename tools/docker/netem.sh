#!/usr/bin/env bash
# Adds or clears artificial delay / jitter / loss on the running server
# container, live, without restarting it.
#
#   tools/docker/netem.sh 100            # +100 ms
#   tools/docker/netem.sh 100 20         # +100 ms, +-20 ms jitter
#   tools/docker/netem.sh 100 20 1       # ... and 1% loss
#   tools/docker/netem.sh off            # back to the real network
#   tools/docker/netem.sh show           # what is applied now
#
# netem shapes the container's outgoing traffic only (server -> client):
#   - the delay adds to every client's RTT once, so "+100" means RTT + 100 ms;
#   - loss drops server -> client packets only (snapshots, acks), not input.
# Jitter reorders packets, as a real jittery link does.
#
# Needs the compose service's cap_add NET_ADMIN and an image with iproute2
# (both in tools/docker). NK_CONTAINER overrides the container name.
set -euo pipefail

CONTAINER="${NK_CONTAINER:-network-kernel-server-1}"
DEVICE=eth0

tc_in_container() {
    docker exec -u root "$CONTAINER" tc "$@"
}

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

is_number() {
    [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]]
}

[ $# -ge 1 ] || usage

case "$1" in
    show)
        tc_in_container qdisc show dev "$DEVICE"
        ;;
    off)
        # Deleting the root qdisc fails when none was added; that is already off.
        tc_in_container qdisc del dev "$DEVICE" root 2>/dev/null || true
        echo "netem off: $(tc_in_container qdisc show dev "$DEVICE")"
        ;;
    *)
        delay_ms="$1"
        jitter_ms="${2:-0}"
        loss_pct="${3:-0}"
        for value in "$delay_ms" "$jitter_ms" "$loss_pct"; do
            is_number "$value" || usage
        done
        args=(delay "${delay_ms}ms")
        if [ "$jitter_ms" != "0" ]; then
            args+=("${jitter_ms}ms")
        fi
        if [ "$loss_pct" != "0" ]; then
            args+=(loss "${loss_pct}%")
        fi
        # replace adds the qdisc the first time and swaps it afterwards.
        tc_in_container qdisc replace dev "$DEVICE" root netem "${args[@]}"
        echo "netem on: $(tc_in_container qdisc show dev "$DEVICE")"
        ;;
esac
