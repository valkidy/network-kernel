#!/usr/bin/env bash
# Bazel --workspace_status_command. STABLE_ keys land in stable-status.txt,
# and any change there reruns every stamp = 1 action that reads it, so the
# kernel's build info follows HEAD without a `bazel clean`.
echo "STABLE_GIT_COMMIT $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "STABLE_GIT_COMMIT_COUNT $(git rev-list --count HEAD 2>/dev/null || echo 0)"
echo "STABLE_GIT_COMMIT_TIMESTAMP $(git show -s --format=%ct HEAD 2>/dev/null || echo unknown)"
