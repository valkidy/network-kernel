---
name: network_example_project_alignment
description: Align code changes with network-example's compact C++20/Bazel workspace, third-party dependency wrappers, platform mapping scaffolding, and smoke-binary conventions.
---

# Goal
Ensure code changes fit this repository's current shape:
- a compact Bazel workspace named `network-example`
- a C++20 smoke binary at `//engine/src:example_app`
- third-party dependencies declared in `WORKSPACE` and wrapped by `third_party/*.BUILD`
- platform mapping scaffolding under `engine/BUILD`, `engine/platforms.bzl`, and `platform_mappings`
- minimal source layout under `engine/src/`

# Decision Priority
1. Inspect nearby files and follow local conventions first.
2. If no strong pattern exists, apply rules below.
3. Keep changes narrow; avoid carrying over unused assumptions from other project templates.

# File Naming Rules
- Use lower_snake_case
- C++ source: `.cc`
- C++ headers: `.h`
- Tests, when introduced: `<base>_test.cc`
- Build wrapper files for external deps: `third_party/<repo>.BUILD`
- Avoid adding platform-specific suffixes unless the code is actually platform-specific.

# Folder Structure
- Keep application/smoke-test source under `engine/src/`.
- Keep platform declarations in `engine/BUILD`, `engine/platforms.bzl`, and `platform_mappings`.
- Keep external dependency wrapper BUILD files under `third_party/`.
- Do not introduce app folders, mobile modules, or large framework-style source trees unless the user explicitly asks for that architecture.

# Bazel Rules
- Prefer explicit `cc_binary`, `cc_library`, or `cc_test` targets with small, readable `srcs`, `hdrs`, `deps`, and `data`.
- Use labels relative to the existing package layout; the current main target is `//engine/src:example_app`.
- Add third-party dependencies by declaring an `http_archive` in `WORKSPACE` and a matching wrapper BUILD file under `third_party/`.
- Keep repository names stable and descriptive; use the existing external labels in `engine/src/BUILD` as the dependency surface.
- Preserve `--noenable_bzlmod` unless the task is specifically to migrate dependency management.
- Use `select()` only when a target genuinely has platform-specific inputs. Do not add platform selects preemptively.

# C++ Style
- Types: PascalCase
- Functions: lower_snake_case unless nearby code establishes otherwise
- Variables: snake_case
- Members: snake_case_
- Constants: kCamelCase
- Prefer standard library types and RAII.
- Keep includes explicit and grouped consistently with nearby files.
- Use `std::uint*_t` from `<cstdint>` for fixed-width integers.
- Avoid broad logging or demo output changes unless they support the user's requested behavior.

# Third-Party Dependency Changes
- Prefer pinned versions with `sha256`.
- Add or update only the wrapper BUILD file needed for the dependency.
- Expose the smallest useful target surface, such as a single `cc_library` for headers or compiled sources.
- Keep patches under `third_party/` and reference them from `WORKSPACE`.
- When changing dependency versions, verify `//engine/src:example_app` still builds.

# Validation
- For code, BUILD, or dependency wrapper changes, run the `bazel-build` skill's helper script when feasible.
- If only this skill text changes, validate by rereading the edited skill files for stale project names, stale platform assumptions, and broken command examples.
