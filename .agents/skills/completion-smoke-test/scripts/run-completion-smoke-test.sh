#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd -P)"
OUTPUT_BASE="${OUTPUT_BASE:-/private/tmp/bazel-network-example-completion-smoke}"
APP_BIN="$REPO_ROOT/bazel-bin/app/app"
ADDRESS="${ADDRESS:-127.0.0.1:7777}"
PORT="${PORT:-7777}"
READY_TIMEOUT_SECONDS="${READY_TIMEOUT_SECONDS:-10}"
LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/network-example-smoke.XXXXXX")"
SERVER_LOG="$LOG_DIR/dedicated_server.log"
CLIENT_LOG="$LOG_DIR/example_client.log"
HOST_LOG="$LOG_DIR/host_server.log"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
    wait "$SERVER_PID" >/dev/null 2>&1 || true
  fi
}

print_failure_context() {
  echo "==> Smoke test failed. Logs are in: $LOG_DIR" >&2
  if [[ -f "$SERVER_LOG" ]]; then
    echo "==> Last dedicated_server log lines:" >&2
    tail -n 40 "$SERVER_LOG" >&2 || true
  fi
  if [[ -f "$CLIENT_LOG" ]]; then
    echo "==> Last example_client log lines:" >&2
    tail -n 40 "$CLIENT_LOG" >&2 || true
  fi
  if [[ -f "$HOST_LOG" ]]; then
    echo "==> Last host_server log lines:" >&2
    tail -n 40 "$HOST_LOG" >&2 || true
  fi
}

on_error() {
  local exit_code=$?
  print_failure_context
  exit "$exit_code"
}

trap cleanup EXIT
trap on_error ERR

if command -v bazelisk >/dev/null 2>&1; then
  BAZEL_CMD="bazelisk"
elif command -v bazel >/dev/null 2>&1; then
  BAZEL_CMD="bazel"
else
  echo "ERROR: bazelisk/bazel not found in PATH" >&2
  exit 127
fi

cd "$REPO_ROOT"

echo "==> Building unified app"
"$BAZEL_CMD" \
  --output_base="$OUTPUT_BASE" \
  build \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //app:app

echo "==> Running GameServer focused tests"
"$BAZEL_CMD" \
  --output_base="$OUTPUT_BASE" \
  test \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //game_server:enemy_ai_controller_test \
  //game_server:enemy_manager_test

if [[ ! -x "$APP_BIN" ]]; then
  echo "ERROR: expected app binary not found or not executable: $APP_BIN" >&2
  exit 1
fi

echo "==> Running host_server mode"
"$APP_BIN" --mode=host_server --port="$PORT" >"$HOST_LOG" 2>&1
if ! grep -Eq "render_state net_id=[0-9]+ type=2 pos=\\(6" "$HOST_LOG"; then
  echo "ERROR: host_server did not render the GameServer enemy" >&2
  print_failure_context
  exit 1
fi
if ! grep -Eq "event type=6 .* net_id=[0-9]+ peer=0 code=0" "$HOST_LOG"; then
  echo "ERROR: host_server did not report an enemy fire event" >&2
  print_failure_context
  exit 1
fi
if ! grep -Eq "event type=8 .* peer=0 code=5" "$HOST_LOG"; then
  echo "ERROR: host_server did not report enemy rifle damage" >&2
  print_failure_context
  exit 1
fi

echo "==> Starting dedicated_server mode"
"$APP_BIN" --mode=dedicated_server --port="$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

ready_deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
while (( SECONDS < ready_deadline )); do
  if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    echo "ERROR: dedicated_server exited before becoming ready" >&2
    wait "$SERVER_PID" || true
    print_failure_context
    exit 1
  fi

  if grep -q "dedicated server listening on 127.0.0.1:$PORT" "$SERVER_LOG"; then
    break
  fi

  sleep 0.2
done

if ! grep -q "dedicated server listening on 127.0.0.1:$PORT" "$SERVER_LOG"; then
  echo "ERROR: timed out waiting for dedicated_server readiness" >&2
  print_failure_context
  exit 1
fi

echo "==> Running client mode against $ADDRESS"
"$APP_BIN" --mode=client --address="$ADDRESS" >"$CLIENT_LOG" 2>&1
sleep 0.2
if ! grep -Eq "event type=4 .* peer=0 " "$SERVER_LOG"; then
  echo "ERROR: dedicated_server did not report a GameServer enemy spawn" >&2
  print_failure_context
  exit 1
fi

echo "==> Completion smoke test passed"
echo "==> Logs are in: $LOG_DIR"
