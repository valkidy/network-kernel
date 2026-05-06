---
name: bazel-build
description: Run network-example's Bazel verification for the unified app binary, engine modules, tests, and third-party dependency wrappers. Use this when the user asks to build, test, verify compilation, or validate Bazel/C++ dependency changes.
---

# Bazel Build Skill

Use this skill when:
- the user asks to build the repo
- the user asks whether a change compiles
- the user asks to run tests or verify the current change
- you need compilation verification before concluding a code task
- a change touches Bazel files, `WORKSPACE`, `.bazelrc`, `platform_mappings`, or `third_party/*.BUILD`
- a change touches C++ code under `engine/src/`, `app/`, or tests

## Current Project Shape

- Main app target: `//app:app`
- Engine module targets live under `//engine/src/<module>:...`
- Primary local config: `--config=macos`
- Bazel version: `.bazelversion` pins Bazel `7.4.1`
- Dependency management currently uses `WORKSPACE`; `.bazelrc` sets `--noenable_bzlmod`
- Checked-in `cc_test` targets live under `//engine/src/tests/...`
- Do not use `//...` as the default build/test verification pattern; it descends into local third-party wrapper packages that are not the app verification surface.
- Do not use bare `bazel query` in this sandbox. The default output base may not be writable, so use the sandbox-friendly output base shown below.

## Platform Selection

Choose the platform in this order:

1. If the user explicitly requests a platform supported by the helper script, use it.
2. Otherwise default to `macos`, which matches the current local development environment.
3. Treat Android, iOS, Windows, and Linux entries in `.bazelrc`, `engine/BUILD`, and `platform_mappings` as inherited platform mapping scaffolding unless the repo grows real targets for them.

## Helper script

The helper script defaults to a macOS build:

```bash
.agents/skills/bazel-build/scripts/run-bazel-build.sh macos
```

Use `test` as the second argument to run the platform's standard test set:

```bash
.agents/skills/bazel-build/scripts/run-bazel-build.sh macos test
```

If there are no test targets under `//engine/src/tests/...`, `test` mode builds `//app:app` instead. The helper uses `OUTPUT_BASE=/private/tmp/bazel-network-example` by default; override that environment variable only when a different writable output base is required.

## Standard Commands

Build the unified app binary:

```bash
bazel build \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //app:app
```

Run checked-in tests:

```bash
bazel test \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  <engine/src test targets from the query commands below>
```

## Query Commands

Use these query commands for target discovery and dependency inspection. They all set `--output_base=/private/tmp/bazel-network-example` because the default Bazel output base may not be readable/writable in the local sandbox.

List C++ rules:

```bash
bazel --output_base=/private/tmp/bazel-network-example query 'kind("cc_.* rule", //...)'
```

List tests:

```bash
bazel --output_base=/private/tmp/bazel-network-example query 'kind("cc_test rule", //...)'
```

List engine tests:

```bash
bazel --output_base=/private/tmp/bazel-network-example query 'kind("cc_test rule", //engine/src/tests/...)'
```

Inspect one layer of dependencies for a target:

```bash
bazel --output_base=/private/tmp/bazel-network-example query 'deps(<target>, 1)'
```

Find reverse dependencies of a target:

```bash
bazel --output_base=/private/tmp/bazel-network-example query 'rdeps(//..., <target>)'
```

## Verification Guidance

- Prefer the helper script for normal verification so the command stays consistent across agents.
- If dependency fetching fails because of restricted network access, request escalation and rerun the same command.
- If no tests exist, report that build verification passed and that there were no repository test targets to exercise.
