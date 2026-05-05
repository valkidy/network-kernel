#!/usr/bin/env bash
set -euo pipefail

PLATFORM="${1:-macos}"
MODE="${2:-build}"

if command -v bazelisk >/dev/null 2>&1; then
  BAZEL_CMD="bazelisk"
elif command -v bazel >/dev/null 2>&1; then
  BAZEL_CMD="bazel"
else
  echo "ERROR: bazelisk/bazel not found in PATH" >&2
  exit 127
fi

case "$MODE" in
  build)
    case "$PLATFORM" in
      macos)
        CMD=(
          "$BAZEL_CMD" build
          --config=macos
          --copt=-Wunused-function
          -c opt
          //engine/src:example_app
        )
        ;;
      *)
        echo "ERROR: unsupported platform '$PLATFORM'. Current local verification supports 'macos'." >&2
        exit 2
        ;;
    esac
    ;;
  test)
    case "$PLATFORM" in
      macos)
        TEST_TARGETS=()
        while IFS= read -r target; do
          if [[ -n "$target" ]]; then
            TEST_TARGETS+=("$target")
          fi
        done < <("$BAZEL_CMD" query 'kind(".*_test rule", //engine/...)' 2>/dev/null || true)
        if [[ "${#TEST_TARGETS[@]}" -gt 0 ]]; then
          CMD=(
            "$BAZEL_CMD" test
            --config=macos
            --copt=-Wunused-function
            -c opt
            "${TEST_TARGETS[@]}"
          )
        else
          echo "==> No test targets found under //engine/...; building smoke binary instead."
          CMD=(
            "$BAZEL_CMD" build
            --config=macos
            --copt=-Wunused-function
            -c opt
            //engine/src:example_app
          )
        fi
        ;;
      *)
        echo "ERROR: unsupported platform '$PLATFORM'. Current local verification supports 'macos'." >&2
        exit 2
        ;;
    esac
    ;;
  *)
    echo "ERROR: unsupported mode '$MODE'. Use 'build' or 'test'." >&2
    exit 2
    ;;
esac

echo "==> Detected platform: $PLATFORM"
echo "==> Mode: $MODE"
echo "==> Running: ${CMD[*]}"
"${CMD[@]}"
