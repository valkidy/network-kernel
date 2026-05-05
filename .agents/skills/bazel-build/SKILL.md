---
name: bazel-build
description: Run network-example's Bazel verification for the C++ engine smoke binary and third-party dependency wrappers. Use this when the user asks to build, test, verify compilation, or validate Bazel/C++ dependency changes.
---

# Bazel Build Skill

Use this skill when:
- the user asks to build the repo
- the user asks whether a change compiles
- the user asks to run tests or verify the current change
- you need compilation verification before concluding a code task
- a change touches Bazel files, `WORKSPACE`, `.bazelrc`, `platform_mappings`, or `third_party/*.BUILD`
- a change touches C++ code under `engine/`

## Current Project Shape

- Main target: `//engine/src:example_app`
- Primary local config: `--config=macos`
- Bazel version: `.bazelversion` pins Bazel `7.4.1`
- Dependency management currently uses `WORKSPACE`; `.bazelrc` sets `--noenable_bzlmod`
- There are no checked-in `cc_test` targets yet
- Do not use `//...` as the default verification pattern; it descends into local third-party wrapper packages that are not the app verification surface.

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

If there are no test targets under `//engine/...`, `test` mode builds `//engine/src:example_app` instead.

## Standard Commands

Build the smoke binary:

```bash
bazel build \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  //engine/src:example_app
```

When tests exist, run engine tests only:

```bash
bazel test \
  --config=macos \
  --copt=-Wunused-function \
  -c opt \
  <test targets from bazel query 'kind(".*_test rule", //engine/...)'>
```

## Verification Guidance

- Prefer the helper script for normal verification so the command stays consistent across agents.
- If dependency fetching fails because of restricted network access, request escalation and rerun the same command.
- If no tests exist, report that build verification passed and that there were no repository test targets to exercise.
