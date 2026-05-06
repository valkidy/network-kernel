#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd -P)"
OUTPUT_BASE="${OUTPUT_BASE:-/private/tmp/bazel-network-example}"
SERVER_BIN="$REPO_ROOT/bazel-bin/app/dedicated_server/dedicated_server"
CLIENT_BIN="$REPO_ROOT/bazel-bin/app/example_client/example_client"
ADDRESS="${ADDRESS:-127.0.0.1:7777}"
READY_TIMEOUT_SECONDS="${READY_TIMEOUT_SECONDS:-10}"
LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/network-example-smoke.XXXXXX")"
SERVER_LOG="$LOG_DIR/dedicated_server.log"
CLIENT_LOG="$LOG_DIR/example_client.log"
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

echo "==> Building dedicated_server and example_client"
"$BAZEL_CMD" \
  --output_base="$OUTPUT_BASE" \
  build \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //app/dedicated_server:dedicated_server \
  //app/example_client:example_client

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "ERROR: expected server binary not found or not executable: $SERVER_BIN" >&2
  exit 1
fi

if [[ ! -x "$CLIENT_BIN" ]]; then
  echo "ERROR: expected client binary not found or not executable: $CLIENT_BIN" >&2
  exit 1
fi

echo "==> Starting dedicated_server"
"$SERVER_BIN" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

ready_deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
while (( SECONDS < ready_deadline )); do
  if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    echo "ERROR: dedicated_server exited before becoming ready" >&2
    wait "$SERVER_PID" || true
    print_failure_context
    exit 1
  fi

  if grep -q "dedicated server listening on 127.0.0.1:7777" "$SERVER_LOG"; then
    break
  fi

  sleep 0.2
done

if ! grep -q "dedicated server listening on 127.0.0.1:7777" "$SERVER_LOG"; then
  echo "ERROR: timed out waiting for dedicated_server readiness" >&2
  print_failure_context
  exit 1
fi

echo "==> Running example_client against $ADDRESS"
"$CLIENT_BIN" "$ADDRESS" >"$CLIENT_LOG" 2>&1

echo "==> Completion smoke test passed"
echo "==> Logs are in: $LOG_DIR"
