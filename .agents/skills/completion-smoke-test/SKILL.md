---
name: completion-smoke-test
description: Run network-example completion smoke tests for finished tasks, including unified app dedicated_server, client, and host_server mode verification. Use when the user asks to complete a task, finish verification, run smoke test, or validate bazel-bin/app/app runtime modes.
---

# Completion Smoke Test Skill

Use this skill before concluding a task when runtime verification is appropriate for `network-example`, especially after changes touching `app/`, `game_server/`, `engine/src/kernel/`, `engine/src/protocol/`, `engine/src/transport/`, or Bazel targets used by the unified app.

## Helper Script

Run the repository smoke helper from the repo root:

```bash
.agents/skills/completion-smoke-test/scripts/run-completion-smoke-test.sh
```

The helper uses `OUTPUT_BASE=/private/tmp/bazel-network-example-completion-smoke` by default. Override `OUTPUT_BASE`, `PORT`, `ADDRESS`, or `READY_TIMEOUT_SECONDS` only when the local environment requires it.

The helper builds and runs:

- `//app:app`
- `//game_server:enemy_ai_controller_test`
- `//game_server:enemy_manager_test`
- `bazel-bin/app/app --mode=host_server --port=7777`
- `bazel-bin/app/app --mode=dedicated_server --port=7777`
- `bazel-bin/app/app --mode=client --address=127.0.0.1:7777`

## Expected Behavior

- The host server mode starts, runs its scripted frames, and exits successfully.
- The host server log includes a server-owned enemy render state (`type=2`), enemy rocket fire confirmation, projectile spawn, and rocket damage.
- The dedicated server mode starts on `127.0.0.1:7777`.
- The client mode connects to that server and exits successfully.
- After the client connects, the dedicated server log includes a server-owned enemy spawn event (`peer=0`).
- The helper exits nonzero if either binary is missing, the server exits early, readiness times out, or the client fails.
- The helper always attempts to stop the background server before exiting.

## Reporting

When reporting results, include whether the helper passed and summarize any failure using the printed server/client log context. If dependency fetching fails because of restricted network access, request escalation and rerun the same helper command.
