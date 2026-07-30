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
staging, gameplay catalog bundle staging, package packing, and Unity batchmode
execution:

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

# Verify package layout/ABI/export symbols against already-staged package files.
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
to build/pack the Unity package without extra options, first perform the Unity
API alignment preflight below, then include at least one `--release-note` by
default so the script can complete its normal release-note and auto-commit
finalization. Prefer a concise bullet inferred from the actual diff, such as
`updates native plugins` for native asset-only staging or `aligns Unity plugin
API with kernel ABI <version>` for visible ABI/API changes. Only omit
`--release-note` when the user explicitly asks for verify-only, no commit, or
`--auto-commit off`.

## Usage Help

To query usage, run:

```bash
.agents/skills/unity-plugin-package-builder/scripts/run-unity-plugin-package-builder.sh --help
```

The help output must include an `Examples:` section with sample dev branches,
release tags, verify-only invocation, and build/package invocation. Dev branches
use `dev-*`, such as `dev-latest`; release tags use `v*`, such as `v0.6.6`.
Branch/ref failure messages should tell users to rerun with `--help` for
allowed names and examples.

## Unity API Alignment Preflight

For `/unity-package` and normal build/pack requests, before invoking the
package builder script:

1. Check the current git ref. Local branches may be `feat-unity-plugin` or
   `dev-*`; GitHub Actions branch refs may be `feat-unity-plugin` or `dev-*`;
   GitHub Actions release tag refs may be `v*`. If the current ref is outside
   those patterns, stop immediately and report the failure reason; do not
   switch branches, edit files, run builds, stage assets, or package.
2. Reference the C++ kernel public API and update the Unity plugin API before
   building the package. Keep the pass targeted:
   - Compare `engine/src/kernel/public/kernel_api.h` and
     `engine/src/kernel/public/kernel_types.h` against
     `plugins/com.network-example.kernel/Runtime/Core/KernelNative.cs`,
     `KernelTypes.cs`, `KernelAbi.cs`, and `Kernel.cs`.
   - Include `game_server/public/game_server_api.h` and
     `game_server/public/game_server_types.h` when GameServer bindings or
     ABI checks are involved.
   - Align managed ABI constants, struct layouts, enums, P/Invoke exports,
     wrapper methods, editor smoke, and managed smoke with the native headers.
   - If the pass finds required native C++/ABI changes, stop and use
     `unity-plugin-plan-guideline` instead of continuing package-builder work.
3. Verify the native shared-library export lists before building:
   - Build the canonical export set from the public `Kernel_*` and
     `GameServer_*` C ABI declarations in `engine/src/kernel/public/kernel_api.h`
     and `game_server/public/game_server_api.h`, and confirm every entry has a
     C++ implementation in `engine/src/kernel/src/kernel_api.cc` or
     `game_server/src/game_server_api.cc`.
   - Compare that set with the macOS `-Wl,-exported_symbol` allowlist in
     `engine/src/kernel/BUILD.bazel` and the Windows exports in
     `engine/src/kernel/network_kernel_exports.def`. Normalize the macOS
     leading underscore before comparison.
   - Require all three sets to match exactly. On any missing implementation,
     missing platform export, or stale extra export, report the mismatched
     symbol and file, then stop before native build, staging, or packaging.
4. After the Unity API and native exports are aligned, run the bundled script
   as the only entry point for build, stage, verify, pack, release-note, and
   optional Unity batchmode work.

## Branch And Safety

- The script and `/unity-package` workflow may run only from local branches
  named `feat-unity-plugin` or `dev-*`, from matching GitHub Actions branch
  refs, or from GitHub Actions release tag refs named `v*`. If the current ref
  is outside those patterns, stop immediately and report the failure reason.
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

Set `BUNDLE_ARTIFACT_DIR=/absolute/path` when CI or a caller needs
`bundle.zip`, `bundle.bytes`, and `bundle_manifest.json` copied to a known
artifact directory. The default is `$OUTPUT_DIR/template-bundle`.

Codesign is mandatory for every mode that creates or updates the macOS dylib:
the Bazel target produces an ad-hoc signed dylib, and `build-native`/`stage`
verify that signature with `codesign --verify`. Windows DLLs are not codesigned.
`verify` and `pack` do not create native binaries.

Default behavior:

1. Require the current git ref to be an allowed package ref:
   `feat-unity-plugin`, `dev-*`, or CI `v*` release tag.
2. Build `//engine/src/kernel:network_kernel_shared` for macOS and
   Windows x86_64. The macOS target returns the Bazel ad-hoc signed dylib.
3. Stage `bazel-bin/engine/src/kernel/signed/libnetwork_kernel.dylib` into
   `plugins/com.network-example.kernel/Assets/Plugins/macOS/`.
4. Stage `bazel-bin/engine/src/kernel/network_kernel.dll` plus the Windows
   x86_64 support DLLs into
   `plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/`.
5. Verify the staged macOS dylib signature with `codesign --verify`.
6. Build `//game_server/gameplay_catalog_bundle:bundle`, copy the generated
   `bundle.zip` byte-for-byte to
   `plugins/com.network-example.kernel/Runtime/Resources/gameplay_catalog_bundle/bundle.bytes`,
   ensure Unity `.meta` companions exist for the resource folder, bundle
   folder, and `bundle.bytes`, and copy bundle artifacts to
   `BUNDLE_ARTIFACT_DIR`.
7. Verify package layout, C/C# ABI version alignment, required exported
   `Kernel_*`/`GameServer_*` symbols for macOS and Windows, and Windows PE32+
   x86-64 DLL shape. Export checks are ABI-aware: the v8 baseline remains
   compatible with the long-lived Unity plugin branch, while ABI 9-16 and
   GameServer ABI 2-3 symbols are required when the native headers report those
   versions.
8. Delete every `.DS_Store` under `plugins/com.network-example.kernel`, then
   pack a clean UPM tarball in
   `plugins/output`.
9. Optionally run Unity batchmode ABI smoke if Unity is auto-detected and the
   local license/headless environment works. Missing or blocked Unity should be
   reported as a clear skip, not a failure, unless the user provided an explicit
   Unity executable path. Override the default smoke timeout with
   `UNITY_TIMEOUT_SECONDS=<seconds>` when diagnosing slow Editor startup.
10. If successful and `--auto-commit on`, prepend the supplied release-note
   bullets to `plugins/com.network-example.kernel/RELEASE_NOTES.md` and commit
   only when the dirty files are limited to staged native plugin assets under
   `Assets/Plugins`, Unity package `.cs` files, the generated
   `Runtime/Resources/gameplay_catalog_bundle/bundle.bytes`, `RELEASE_NOTES.md`,
   generated resource `.meta` companions, and Unity's generated
   `RELEASE_NOTES.md.meta`.

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

On any failure, stop immediately and lead the user-facing response with the
literal Markdown H2 heading `## Work failed`. Match the heading level, spacing,
and concise wording used by `## Work completed`; do not bury it below logs or a
general summary. Put one blank line after the heading. Present a single-line
error as inline code in its own paragraph, and use a fenced `text` block only
for a multi-line excerpt. Follow this compact structure:

```md
## Work failed

The Unity package was not produced. <completed work>, but <failed step>:

`<most relevant single error line>`

<cause or scope conclusion, only when supported by evidence>

Packaging stopped before <stages that did not run>.
```

Use `The Unity package workflow failed after producing: <path>` instead when a
tarball was produced before a later step failed. State which earlier stages
completed and which later stages did not run. Preserve the most actionable
native, linker, ABI, packaging, or Unity error line verbatim. Only claim that a
failure also exists on `main`, or is unrelated to the current changes, after
actually verifying that conclusion; otherwise omit it. Do not update release
notes or auto-commit after a failed required stage.

On success, lead with the matching Markdown H2 heading `## Work completed`, put
one blank line after it, and report:

- Final `.tgz` path when packing runs.
- Staged macOS dylib and Windows DLL paths when staging runs.
- Verification status.
- Whether Unity smoke passed, skipped, or failed.
- Release-note path and auto-commit result when finalization runs.
- Gameplay catalog bundle resource path and bundle artifact paths when staging
  or packing runs.
