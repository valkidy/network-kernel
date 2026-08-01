# Multi-Bone Locomotion Development Setup

Phase 0 uses macOS arm64 as the required development and verification platform.
The setup is intentionally preflight-driven: repository scripts diagnose the
machine but do not install packages or modify the Unity installation.

## Required tools

- macOS on Apple Silicon.
- Apple Command Line Tools with a C++20-capable `clang++`.
- Bazel 7.4.1, matching `.bazelversion`.
- Git, Node.js, npm, curl, and `shasum`.
- Homebrew `openssl@3` libraries under `/opt/homebrew/opt/openssl@3/lib`.

Typical manual installation commands are:

```bash
xcode-select --install
brew install bazelisk node openssl@3
```

Unity 6000.4.3f1 is optional during Phase 0. Unity package source restoration,
Editor licensing, and batchmode validation are deferred to Phase 5.

ozz-animation 0.16.0 uses its reference scalar math implementation on arm64
because that upstream release leaves its NEON path disabled. This is accepted
for the experiment baseline; locomotion profiling must record it before any
performance conclusion is made.

## Verify the environment

From the repository root:

```bash
tools/locomotion_dev_preflight.sh
```

The script exits non-zero when a required check fails. A missing Unity Editor
is reported as a warning only.

## Phase 0 Bazel gates

Run the focused ozz dependency smoke first, then the existing application
baseline:

```bash
bazel --output_base=/private/tmp/bazel-network-example-phase0 test \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //tools:ozz_dependency_smoke_test

bazel --output_base=/private/tmp/bazel-network-example-phase0 build \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //app:app
```

Do not use `bazel test //...` for this phase. The required gate is limited to
the imported ozz runtime/converter surface and the unified app regression.
