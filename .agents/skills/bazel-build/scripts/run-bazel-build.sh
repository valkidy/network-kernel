#!/usr/bin/env bash
set -euo pipefail

PLATFORM="${1:-macos}"
MODE="${2:-build}"
OUTPUT_BASE="${OUTPUT_BASE:-/private/tmp/bazel-network-example}"

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
          "$BAZEL_CMD"
          --output_base="$OUTPUT_BASE"
          build
          --config=macos
          --copt=-Wunused-function
          -c opt
          //app:app
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
        done < <("$BAZEL_CMD" --output_base="$OUTPUT_BASE" query 'kind("cc_test rule", //engine/src/tests/...)' 2>/dev/null || true)
        if [[ "${#TEST_TARGETS[@]}" -gt 0 ]]; then
          CMD=(
            "$BAZEL_CMD"
            --output_base="$OUTPUT_BASE"
            test
            --config=macos
            --copt=-Wunused-function
            -c opt
            "${TEST_TARGETS[@]}"
          )
        else
          echo "==> No test targets found under //engine/src/tests/...; building app binary instead."
          CMD=(
            "$BAZEL_CMD"
            --output_base="$OUTPUT_BASE"
            build
            --config=macos
            --copt=-Wunused-function
            -c opt
            //app:app
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
