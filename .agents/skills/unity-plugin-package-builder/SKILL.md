---
name: unity-plugin-package-builder
description: Use when building, staging, packing, or validating the network-example Unity UPM package under plugins/com.network-example.kernel; also trigger for $unity-plugin-package-builder, /unity-package, build Unity package, stage native plugin, or pack com.network-example.kernel.
---

# Unity Plugin Package Builder

Use this skill to build and validate `plugins/com.network-example.kernel` as a
Unity Package Manager package. The deliverable is a UPM `.tgz`, not a
`.unitypackage`; `.unitypackage` is for legacy Asset Package workflows.

## Required Entry Point

Use the bundled script as the only entry point for native build, dylib staging,
package packing, and Unity batchmode execution:

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
  --release-note "updates macOS native plugin"

# Verify package layout/ABI/export symbols without building or staging.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh --mode verify --unity off

# Build and pack without launching Unity, still using the default release-note
# and auto-commit flow.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh \
  --unity off \
  --release-note "updates macOS native plugin"

# Build, update release notes, and auto-commit eligible package changes with
# explicit user- or task-supplied bullets.
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh \
  --release-note "fixes managed host startup" \
  --release-note "updates macOS native plugin"
```

When the user invokes `/unity-package`, `$unity-plugin-package-builder`, or asks
to build/pack the Unity package without extra options, include at least one
`--release-note` by default so the script can complete its normal release-note
and auto-commit finalization. Prefer a concise bullet inferred from the actual
diff, such as `updates macOS native plugin` for dylib-only staging or `adds HP
and MaxHP to Unity render states` for visible ABI/API changes. Only omit
`--release-note` when the user explicitly asks for verify-only, no commit, or
`--auto-commit off`.

## Branch And Safety

- The script must run on `feat-unity-plugin`; it stops immediately on any other
  branch and reports the current branch.
- Stop and ask before switching branches.
- Stop and ask before overwriting unrelated dirty files under
  `plugins/com.network-example.kernel/`, `engine/src/kernel/`, or this skill.
- Native C++/ABI changes are out of scope for package-builder work. If a task
  requires ABI changes, use `unity-plugin-plan-guideline` first.

## Script Contract

The script supports:

- `--mode all|build-native|stage|verify|pack`
- `--platform macos`
- `--unity auto|off|/absolute/path/to/Unity`
- `--output-dir /absolute/path`
- `--release-note "bullet text"`; repeat for each bullet to prepend to
  `plugins/com.network-example.kernel/RELEASE_NOTES.md`
- `--auto-commit on|off`; defaults to `on`

Default behavior:

1. Build `//engine/src/kernel:network_kernel_shared` for macOS.
2. Stage `bazel-bin/engine/src/kernel/libnetwork_kernel.dylib` into
   `plugins/com.network-example.kernel/Assets/Plugins/macOS/`.
3. Ad-hoc sign the staged dylib and remove any GateKeeper quarantine attribute.
4. Verify package layout, C/C# ABI version alignment, and required exported
   `Kernel_*` symbols.
5. Pack a clean UPM tarball in
   `plugins/output`.
6. Optionally run Unity batchmode ABI smoke if Unity is auto-detected and the
   local license/headless environment works. Missing or blocked Unity should be
   reported as a clear skip, not a failure, unless the user provided an explicit
   Unity executable path. Override the default smoke timeout with
   `UNITY_TIMEOUT_SECONDS=<seconds>` when diagnosing slow Editor startup.
7. If successful and `--auto-commit on`, prepend the supplied release-note
   bullets to `plugins/com.network-example.kernel/RELEASE_NOTES.md` and commit
   only when the dirty files are limited to the staged dylib, Unity package
   `.cs` files, and `RELEASE_NOTES.md`.

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
- If any dirty file falls outside the allowlist, the script skips the commit and
  lists the blocking paths. It does not stage unrelated files.
- Use `--auto-commit off` when validating or packing without changing release
  notes or creating a commit.

## Reporting

Report:

- Final `.tgz` path when packing runs.
- Staged dylib path when staging runs.
- Verification status.
- Whether Unity smoke passed, skipped, or failed.
- Release-note path and auto-commit result when finalization runs.
