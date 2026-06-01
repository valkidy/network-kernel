---
name: unity-plugin-package-builder
description: Use when building, staging, packing, or validating the network-example Unity UPM package under plugins/com.network-example.kernel; also trigger for $unity-plugin-package-builder, /unity-package, build Unity package, stage native plugin, or pack com.network-example.kernel.
---

# Unity Plugin Package Builder

Use this skill to build and validate `plugins/com.network-example.kernel` as a
Unity Package Manager package. The deliverable is a UPM `.tgz`, not a
`.unitypackage`; `.unitypackage` is for legacy Asset Package workflows.

## Required Entry Point

Use the bundled script as the only entry point for native build, native plugin
staging, package packing, and Unity batchmode execution:

```bash
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh
```

Do not hand-roll equivalent Bazel, copy, `npm pack`, or Unity commands unless
you are editing this script itself.

Common invocations:

```bash
# Default /unity-package behavior: build, stage, verify, pack, update release notes,
# and auto-commit eligible package changes. Infer concise release-note bullets from
# the actual package/native changes when the user does not provide them.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh \
  --release-note "updates native plugins"

# Verify package layout/ABI/export symbols without building or staging.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh --mode verify --unity off

# Build and pack without launching Unity, still using the default release-note
# and auto-commit flow.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh \
  --unity off \
  --release-note "updates native plugins"

# Build, update release notes, and auto-commit eligible package changes with
# explicit user- or task-supplied bullets.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh \
  --release-note "fixes managed host startup" \
  --release-note "updates native plugins"
```

When the user invokes `/unity-package`, `$unity-plugin-package-builder`, or asks
to build/pack the Unity package without extra options, include at least one
`--release-note` by default so the script can complete its normal release-note
and auto-commit finalization. Prefer a concise bullet inferred from the actual
diff, such as `updates native plugins` for native asset-only staging or `adds
HP and MaxHP to Unity render states` for visible ABI/API changes. Only omit
`--release-note` when the user explicitly asks for verify-only, no commit, or
`--auto-commit off`.

## Branch And Safety

- The script must run on `feat-unity-plugin`; it stops immediately on any other
  branch and reports the current branch.
- For compatibility testing only, integration branches named `integration/*`
  may run the script when
  `UNITY_PACKAGE_BUILDER_ALLOW_INTEGRATION_BRANCH=1` is set. This override
  requires `--auto-commit off` and must not be used for official package
  publishing.
- Stop and ask before switching branches.
- Stop and ask before overwriting unrelated dirty files under
  `plugins/com.network-example.kernel/`, `engine/src/kernel/`, or this skill.
- Native C++/ABI changes are out of scope for package-builder work. If a task
  requires ABI changes, use `unity-plugin-plan-guideline` first.

## Script Contract

The script supports:

- `--mode all|build-native|stage|verify|pack`
- `--platform all|macos|windows-x86_64`; defaults to `all`
- `--unity auto|off|/absolute/path/to/Unity`
- `--output-dir /absolute/path`
- `--release-note "bullet text"`; repeat for each bullet to prepend to
  `plugins/com.network-example.kernel/RELEASE_NOTES.md`
- `--auto-commit on|off`; defaults to `on`

Codesign is mandatory for every mode that creates or updates the macOS dylib:
the Bazel target produces an ad-hoc signed dylib, and `build-native`/`stage`
verify that signature with `codesign --verify`. Windows DLLs are not codesigned.
`verify` and `pack` do not create native binaries.

Default behavior:

1. Build `//engine/src/kernel:network_kernel_shared` for macOS and
   Windows x86_64. The macOS target returns the Bazel ad-hoc signed dylib.
2. Stage `bazel-bin/engine/src/kernel/signed/libnetwork_kernel.dylib` into
   `plugins/com.network-example.kernel/Assets/Plugins/macOS/`.
3. Stage `bazel-bin/engine/src/kernel/network_kernel.dll` plus the Windows
   x86_64 support DLLs into
   `plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/`.
4. Verify the staged macOS dylib signature with `codesign --verify`.
5. Verify package layout, C/C# ABI version alignment, required exported
   `Kernel_*`/`GameServer_*` symbols for macOS and Windows, and Windows PE32+
   x86-64 DLL shape. Export checks are ABI-aware: the v8 baseline remains
   compatible with the long-lived Unity plugin branch, while ABI 9-12 and
   GameServer ABI 2 symbols are required when the native headers report those
   versions.
6. Delete every `.DS_Store` under `plugins/com.network-example.kernel`, then
   pack a clean UPM tarball in
   `plugins/output`.
7. Optionally run Unity batchmode ABI smoke if Unity is auto-detected and the
   local license/headless environment works. Missing or blocked Unity should be
   reported as a clear skip, not a failure, unless the user provided an explicit
   Unity executable path. Override the default smoke timeout with
   `UNITY_TIMEOUT_SECONDS=<seconds>` when diagnosing slow Editor startup.
8. If successful and `--auto-commit on`, prepend the supplied release-note
   bullets to `plugins/com.network-example.kernel/RELEASE_NOTES.md` and commit
   only when the dirty files are limited to staged native plugin assets under
   `Assets/Plugins`, Unity package `.cs` files, `RELEASE_NOTES.md`, and
   Unity's generated `RELEASE_NOTES.md.meta`.

Auto commit details:

- Commit message is `feat: bump Unity package to <package.json version>`.
- Release notes use a single cumulative file. The newest block is inserted at
  the top:

  ```md
  0.6.4 release notes:

  - fixes managed host startup
  - updates macOS native plugin
  ```

- If package files changed but no `--release-note` was supplied, the script
  skips the commit and tells the user to rerun with release-note bullets. Treat
  that as a caller error for normal `/unity-package` runs; rerun with inferred
  release-note bullets unless the user explicitly requested no finalization.
- Treat `plugins/com.network-example.kernel/RELEASE_NOTES.md.meta` as an
  eligible release-note companion file because Unity may generate it when the
  package is imported.
- If any dirty file falls outside the allowlist, the script skips the commit and
  lists the blocking paths. It does not stage unrelated files.
- Use `--auto-commit off` when validating or packing without changing release
  notes or creating a commit.

## Reporting

Report:

- Final `.tgz` path when packing runs.
- Staged macOS dylib and Windows DLL paths when staging runs.
- Verification status.
- Whether Unity smoke passed, skipped, or failed.
- Release-note path and auto-commit result when finalization runs.
