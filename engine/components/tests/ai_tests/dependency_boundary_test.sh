#!/usr/bin/env bash
set -euo pipefail

ai_build="${TEST_SRCDIR}/${TEST_WORKSPACE}/engine/components/ai/BUILD.bazel"

if grep -E '"//engine/src|"//game_server|"//app' "${ai_build}" >/dev/null; then
  echo "engine/components/ai must not depend on engine/src, game_server, or app"
  exit 1
fi

for engine_build in "${TEST_SRCDIR}/${TEST_WORKSPACE}"/engine/src/*/BUILD.bazel; do
  if grep -F '"//engine/components/ai' "${engine_build}" >/dev/null; then
    echo "engine/src modules must not depend on engine/components/ai"
    echo "Found forbidden dependency in ${engine_build}"
    exit 1
  fi
done

exit 0
