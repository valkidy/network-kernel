---
name: completion-smoke-test
description: Run network-example completion smoke tests for finished tasks, including dedicated_server and example_client localhost verification. Use when the user asks to complete a task, finish verification, run smoke test, or validate bazel-bin/app/dedicated_server/dedicated_server with bazel-bin/app/example_client/example_client.
---

# Completion Smoke Test Skill

Use this skill before concluding a task when runtime verification is appropriate for `network-example`, especially after changes touching `app/`, `kernel/`, `protocol/`, `transport/`, or Bazel targets used by the dedicated server or example client.

## Helper Script

Run the repository smoke helper from the repo root:

```bash
.agents/skills/completion-smoke-test/scripts/run-completion-smoke-test.sh
```

The helper builds and runs:

- `//app/dedicated_server:dedicated_server`
- `//app/example_client:example_client`
- `bazel-bin/app/dedicated_server/dedicated_server`
- `bazel-bin/app/example_client/example_client 127.0.0.1:7777`

## Expected Behavior

- The server starts on `127.0.0.1:7777`.
- The client connects to that server and exits successfully.
- The helper exits nonzero if either binary is missing, the server exits early, readiness times out, or the client fails.
- The helper always attempts to stop the background server before exiting.

## Reporting

When reporting results, include whether the helper passed and summarize any failure using the printed server/client log context. If dependency fetching fails because of restricted network access, request escalation and rerun the same helper command.
