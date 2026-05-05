# Pre-commit Checklist

## Scope
- change is focused on the requested C++/Bazel/dependency behavior
- no unused assumptions from other project templates were added

## File Layout
- C++ source stays under `engine/src/` unless a broader layout is explicitly needed
- external dependency wrappers stay under `third_party/`
- platform mapping scaffolding stays in `engine/BUILD`, `engine/platforms.bzl`, and `platform_mappings`

## Naming
- file names are lower_snake_case
- C++ files use `.cc` and `.h`
- future tests use `<base>_test.cc`

## BUILD
- target labels match the current package layout
- dependencies are explicit and minimally exposed
- `select()` is used only for real platform-specific inputs
- `WORKSPACE` dependency changes have pinned versions and `sha256`

## C++
- follows nearby style
- uses C++20-compatible code
- uses fixed-width integers from `<cstdint>` when exact width matters
- keeps includes explicit

## Tests
- build verification considered through `.agents/skills/bazel-build/scripts/run-bazel-build.sh macos`
- if tests are added, use engine-scoped Bazel test targets rather than defaulting to `//...`
