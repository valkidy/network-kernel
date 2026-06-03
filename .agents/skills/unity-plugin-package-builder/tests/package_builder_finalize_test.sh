#!/usr/bin/env bash
set -euo pipefail

SCRIPT_UNDER_TEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/run-unity-plugin-package-builder.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  if [[ "$haystack" != *"$needle"* ]]; then
    fail "expected output to contain '$needle'; got: $haystack"
  fi
}

write_file() {
  local path="$1"
  mkdir -p "$(dirname "$path")"
  cat > "$path"
}

make_fake_command_dir() {
  local bin_dir="$1"
  mkdir -p "$bin_dir"

  write_file "$bin_dir/bazel" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

for arg in "$@"; do
  if [[ "$arg" == "build" ]]; then
    if [[ -n "${BAZEL_LOG:-}" ]]; then
      printf '%s\n' "$*" >> "$BAZEL_LOG"
    fi
    mkdir -p "$FAKE_BAZEL_BIN/engine/src/kernel/signed"
    case " $* " in
      *" --config=macos "*)
        echo "fake built dylib" > "$FAKE_BAZEL_BIN/engine/src/kernel/signed/libnetwork_kernel.dylib"
        ;;
      *" --config=mingw_w64 "*)
        mkdir -p "$FAKE_BAZEL_BIN/engine/src/kernel"
        echo "fake built dll" > "$FAKE_BAZEL_BIN/engine/src/kernel/network_kernel.dll"
        ;;
      *)
        echo "fake bazel build missing supported --config: $*" >&2
        exit 1
        ;;
    esac
    exit 0
  fi
  if [[ "$arg" == "info" ]]; then
    printf '%s\n' "$FAKE_BAZEL_BIN"
    exit 0
  fi
done

echo "fake bazel received unsupported arguments: $*" >&2
exit 1
SH
  cp "$bin_dir/bazel" "$bin_dir/bazelisk"

  write_file "$bin_dir/codesign" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ -n "${CODESIGN_LOG:-}" ]]; then
  printf '%s\n' "$*" >> "$CODESIGN_LOG"
fi
exit 0
SH

  write_file "$bin_dir/nm" <<'SH'
#!/usr/bin/env bash
cat <<'SYMBOLS'
00000000 T _Kernel_Create
00000000 T _Kernel_Destroy
00000000 T _Kernel_GetAbiInfo
00000000 T _Kernel_GetLocalPlayerInfo
00000000 T _Kernel_StartClient
00000000 T _Kernel_StartListenServer
00000000 T _Kernel_StartDedicatedServer
00000000 T _Kernel_Update
00000000 T _Kernel_SubmitInput
00000000 T _Kernel_ServerSubmitEntityInput
00000000 T _Kernel_GetRenderStates
00000000 T _Kernel_GetRenderStatesAtTime
00000000 T _Kernel_PollEvents
00000000 T _Kernel_ServerCreateEntity
00000000 T _Kernel_ServerDestroyEntity
00000000 T _Kernel_ServerSetEntityTransform
00000000 T _Kernel_ServerSetEntityVelocity
00000000 T _Kernel_ServerSetEntityState
00000000 T _Kernel_ServerGetEntityState
00000000 T _Kernel_ServerQueryEntities
00000000 T _GameServer_GetAbiInfo
00000000 T _GameServer_Create
00000000 T _GameServer_CreateWithWeaponTemplateDirectory
00000000 T _GameServer_Destroy
00000000 T _GameServer_HandleEvent
00000000 T _GameServer_Tick
00000000 T _GameServer_GetEnemyCount
00000000 T _GameServer_QueryWeaponTemplate
00000000 T _GameServer_DespawnAll
SYMBOLS
SH

  write_file "$bin_dir/x86_64-w64-mingw32-objdump" <<'SH'
#!/usr/bin/env bash
cat <<'SYMBOLS'
Export Table:
	[   0] Kernel_Create
	[   1] Kernel_Destroy
	[   2] Kernel_GetAbiInfo
	[   3] Kernel_GetLocalPlayerInfo
	[   4] Kernel_StartClient
	[   5] Kernel_StartListenServer
	[   6] Kernel_StartDedicatedServer
	[   7] Kernel_Update
	[   8] Kernel_SubmitInput
	[   9] Kernel_ServerSubmitEntityInput
	[  10] Kernel_GetRenderStates
	[  11] Kernel_GetRenderStatesAtTime
	[  12] Kernel_PollEvents
	[  13] Kernel_ServerCreateEntity
	[  14] Kernel_ServerDestroyEntity
	[  15] Kernel_ServerSetEntityTransform
	[  16] Kernel_ServerSetEntityVelocity
	[  17] Kernel_ServerSetEntityState
	[  18] Kernel_ServerGetEntityState
	[  19] Kernel_ServerQueryEntities
	[  20] GameServer_GetAbiInfo
	[  21] GameServer_Create
	[  22] GameServer_CreateWithWeaponTemplateDirectory
	[  23] GameServer_Destroy
	[  24] GameServer_HandleEvent
	[  25] GameServer_Tick
	[  26] GameServer_GetEnemyCount
	[  27] GameServer_QueryWeaponTemplate
	[  28] GameServer_DespawnAll
Import Table:
SYMBOLS
SH

  write_file "$bin_dir/file" <<'SH'
#!/usr/bin/env bash
for path in "$@"; do
  printf '%s: PE32+ executable (DLL) x86-64, for MS Windows\n' "$path"
done
SH

  write_file "$bin_dir/npm" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
destination=""
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --pack-destination)
      destination="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done
[[ -n "$destination" ]] || destination="$PWD"
mkdir -p "$destination"
touch "$destination/com.network-example.kernel-0.6.4.tgz"
printf '%s\n' "com.network-example.kernel-0.6.4.tgz"
SH

  for command_name in rsync tar; do
    write_file "$bin_dir/$command_name" <<'SH'
#!/usr/bin/env bash
exit 0
SH
  done

  write_file "$bin_dir/xattr" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ -n "${XATTR_LOG:-}" ]]; then
  printf '%s\n' "$*" >> "$XATTR_LOG"
fi
exit 0
SH

  chmod +x "$bin_dir"/*
}

make_fake_repo() {
  local repo_dir="$1"
  local branch="$2"

  mkdir -p "$repo_dir"
  git -C "$repo_dir" init -q -b "$branch"
  git -C "$repo_dir" config user.email "builder-test@example.test"
  git -C "$repo_dir" config user.name "Builder Test"

  touch "$repo_dir/WORKSPACE"
  write_file "$repo_dir/plugins/com.network-example.kernel/package.json" <<'JSON'
{
  "name": "com.network-example.kernel",
  "version": "0.6.4"
}
JSON

  mkdir -p \
    "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/macOS" \
    "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64" \
    "$repo_dir/plugins/com.network-example.kernel/Editor" \
    "$repo_dir/plugins/com.network-example.kernel/Runtime/Core" \
    "$repo_dir/plugins/com.network-example.kernel/Runtime/Client" \
    "$repo_dir/plugins/com.network-example.kernel/Runtime/Host" \
    "$repo_dir/plugins/com.network-example.kernel/Samples~" \
    "$repo_dir/plugins/com.network-example.kernel/Tests~/AbiSmoke" \
    "$repo_dir/engine/src/kernel/public" \
    "$repo_dir/game_server/public"

  echo "fake dylib" > "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib"
  echo "fake dll" > "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/network_kernel.dll"
  for dll_name in libcrypto-4-x64.dll libssl-4-x64.dll libstdc++-6.dll libwinpthread-1.dll; do
    echo "fake support dll" > "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/$dll_name"
  done
  echo "#define KERNEL_ABI_VERSION 7u" > "$repo_dir/engine/src/kernel/public/kernel_types.h"
  echo "#define GAME_SERVER_ABI_VERSION 3u" > "$repo_dir/game_server/public/game_server_types.h"

  write_file "$repo_dir/plugins/com.network-example.kernel/Runtime/Core/KernelTypes.cs" <<'CS'
namespace NetworkExample.Kernel {
  public static class KernelConstants {
    public const uint AbiVersion = 7;
  }
}
CS
  write_file "$repo_dir/plugins/com.network-example.kernel/Runtime/Core/GameServerTypes.cs" <<'CS'
namespace NetworkExample.Kernel {
  public static class GameServerConstants {
    public const uint AbiVersion = 3;
  }
}
CS

  write_file "$repo_dir/plugins/com.network-example.kernel/Runtime/Core/KernelNative.cs" <<'CS'
namespace NetworkExample.Kernel {
  internal static class KernelNative {
    internal const string LibraryName = "network_kernel";
  }
}
CS

  for cs_file in \
    Runtime/Core/GameServerNative.cs \
    Runtime/Client/NetworkClient.cs \
    Runtime/Host/NetworkHost.cs \
    Editor/NetworkKernelAbiSmokeRunner.cs \
    Tests~/AbiSmoke/NetworkKernelManagedAbiSmoke.cs; do
    echo "namespace NetworkExample.Kernel {}" > "$repo_dir/plugins/com.network-example.kernel/$cs_file"
  done

  git -C "$repo_dir" add .
  git -C "$repo_dir" commit -q -m "initial fake package"
}

run_builder() {
  local repo_dir="$1"
  local output_file="$2"
  shift 2

  local sandbox_dir
  sandbox_dir="$(dirname "$repo_dir")"
  local fake_bin="$sandbox_dir/bin"
  local fake_openssl="$sandbox_dir/openssl/lib"
  local fake_mingw_runtime="$sandbox_dir/mingw-runtime"
  mkdir -p "$fake_openssl" "$fake_mingw_runtime"
  touch "$fake_openssl/libssl.3.dylib" "$fake_openssl/libcrypto.3.dylib"
  echo "fake openssl dll" > "$fake_openssl/libcrypto-4-x64.dll"
  echo "fake openssl dll" > "$fake_openssl/libssl-4-x64.dll"
  echo "fake mingw dll" > "$fake_mingw_runtime/libstdc++-6.dll"
  echo "fake mingw dll" > "$fake_mingw_runtime/libwinpthread-1.dll"
  make_fake_command_dir "$fake_bin"

  (
    cd "$repo_dir"
    PATH="$fake_bin:$PATH" \
      OPENSSL_LIB_DIR="$fake_openssl" \
      OPENSSL_MINGW_DLL_DIR="$fake_openssl" \
      MINGW_RUNTIME_DLL_DIR="$fake_mingw_runtime" \
      FAKE_BAZEL_BIN="$sandbox_dir/bazel-bin" \
      BAZEL_LOG="$sandbox_dir/bazel.log" \
      CODESIGN_LOG="$sandbox_dir/codesign.log" \
      XATTR_LOG="$sandbox_dir/xattr.log" \
      "$SCRIPT_UNDER_TEST" "$@"
  ) >"$output_file" 2>&1
}

test_requires_feat_unity_plugin_branch() {
  local sandbox_dir repo_dir output_file status
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-branch.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  make_fake_repo "$repo_dir" "other-branch"

  set +e
  run_builder "$repo_dir" "$output_file" --mode verify --unity off
  status="$?"
  set -e

  [[ "$status" -ne 0 ]] || fail "expected branch guard to fail outside feat-unity-plugin"
  assert_contains "$(cat "$output_file")" "feat-unity-plugin"
}

test_release_notes_are_prepended_and_auto_committed_for_allowed_files() {
  local sandbox_dir repo_dir output_file notes_file notes_meta_file subject
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-commit.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  notes_file="$repo_dir/plugins/com.network-example.kernel/RELEASE_NOTES.md"
  notes_meta_file="$repo_dir/plugins/com.network-example.kernel/RELEASE_NOTES.md.meta"
  make_fake_repo "$repo_dir" "feat-unity-plugin"

  write_file "$notes_file" <<'MD'
0.6.3 release notes:

- previous fix
MD
  git -C "$repo_dir" add "$notes_file"
  git -C "$repo_dir" commit -q -m "docs: add previous release notes"

  echo "// allowed managed change" >> "$repo_dir/plugins/com.network-example.kernel/Runtime/Client/NetworkClient.cs"
  write_file "$notes_meta_file" <<'YAML'
fileFormatVersion: 2
guid: 11111111111111111111111111111111
YAML

  run_builder "$repo_dir" "$output_file" \
    --mode verify \
    --unity off \
    --release-note "fixes managed host startup" \
    --release-note "updates macOS native plugin"

  subject="$(git -C "$repo_dir" log -1 --pretty=%s)"
  [[ "$subject" == "feat: bump Unity package to 0.6.4" ]] || fail "unexpected commit subject: $subject"

  assert_contains "$(sed -n '1,8p' "$notes_file")" "0.6.4 release notes:"
  assert_contains "$(sed -n '1,8p' "$notes_file")" "- fixes managed host startup"
  assert_contains "$(sed -n '1,8p' "$notes_file")" "- updates macOS native plugin"
  assert_contains "$(cat "$notes_file")" "0.6.3 release notes:"
  git -C "$repo_dir" cat-file -e HEAD:plugins/com.network-example.kernel/RELEASE_NOTES.md.meta || fail "expected RELEASE_NOTES.md.meta to be auto committed"
  [[ -z "$(git -C "$repo_dir" status --short)" ]] || fail "expected clean fake repo after auto commit"
}

test_auto_commit_skips_when_disallowed_files_are_dirty() {
  local sandbox_dir repo_dir output_file subject status_output
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-disallowed.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  make_fake_repo "$repo_dir" "feat-unity-plugin"

  echo "// allowed managed change" >> "$repo_dir/plugins/com.network-example.kernel/Runtime/Client/NetworkClient.cs"
  node -e "const fs=require('fs'); const p='$repo_dir/plugins/com.network-example.kernel/package.json'; const json=JSON.parse(fs.readFileSync(p)); json.description='dirty'; fs.writeFileSync(p, JSON.stringify(json, null, 2) + '\n');"

  run_builder "$repo_dir" "$output_file" \
    --mode verify \
    --unity off \
    --release-note "updates macOS native plugin"

  subject="$(git -C "$repo_dir" log -1 --pretty=%s)"
  [[ "$subject" == "initial fake package" ]] || fail "auto commit should have skipped; latest subject was: $subject"
  assert_contains "$(cat "$output_file")" "Auto commit skipped"
  assert_contains "$(cat "$output_file")" "plugins/com.network-example.kernel/package.json"
  status_output="$(git -C "$repo_dir" status --short)"
  assert_contains "$status_output" "plugins/com.network-example.kernel/package.json"
  assert_contains "$status_output" "plugins/com.network-example.kernel/RELEASE_NOTES.md"
}

test_auto_commit_skips_without_release_note_bullets() {
  local sandbox_dir repo_dir output_file subject
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-no-notes.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  make_fake_repo "$repo_dir" "feat-unity-plugin"

  echo "// allowed managed change" >> "$repo_dir/plugins/com.network-example.kernel/Runtime/Client/NetworkClient.cs"

  run_builder "$repo_dir" "$output_file" --mode verify --unity off

  subject="$(git -C "$repo_dir" log -1 --pretty=%s)"
  [[ "$subject" == "initial fake package" ]] || fail "auto commit should have skipped without release notes"
  assert_contains "$(cat "$output_file")" "--release-note"
}

test_build_native_verifies_bazel_signed_dylib() {
  local sandbox_dir repo_dir output_file bazel_log codesign_log macos_build windows_build
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-build-sign.XXXXXX")"
  sandbox_dir="$(cd "$sandbox_dir" && pwd)"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  bazel_log="$sandbox_dir/bazel.log"
  codesign_log="$sandbox_dir/codesign.log"
  make_fake_repo "$repo_dir" "feat-unity-plugin"

  run_builder "$repo_dir" "$output_file" --mode build-native --unity off --auto-commit off

  [[ -f "$bazel_log" ]] || fail "expected build-native mode to invoke Bazel"
  macos_build="$(grep -F -- "--config=macos" "$bazel_log")"
  windows_build="$(grep -F -- "--config=mingw_w64" "$bazel_log")"
  assert_contains "$macos_build" "//engine/src/kernel:network_kernel_shared"
  assert_contains "$windows_build" "//engine/src/kernel:network_kernel_shared"
  if grep -q "network_kernel_shared_codesigned" "$bazel_log"; then
    fail "expected package builder to use the public network_kernel_shared target"
  fi

  [[ -f "$codesign_log" ]] || fail "expected build-native mode to verify the built dylib signature"
  assert_contains "$(cat "$codesign_log")" "--verify --verbose=4 $sandbox_dir/bazel-bin/engine/src/kernel/signed/libnetwork_kernel.dylib"
  if grep -q "network_kernel.dll" "$codesign_log"; then
    fail "expected Windows DLL to skip codesign"
  fi
}

test_stage_verifies_staged_dylib_from_existing_build_output() {
  local sandbox_dir repo_dir output_file codesign_log xattr_log built_dylib built_dll staged_dylib staged_dll status
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-stage-sign.XXXXXX")"
  sandbox_dir="$(cd "$sandbox_dir" && pwd)"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  codesign_log="$sandbox_dir/codesign.log"
  xattr_log="$sandbox_dir/xattr.log"
  built_dylib="$sandbox_dir/bazel-bin/engine/src/kernel/signed/libnetwork_kernel.dylib"
  built_dll="$sandbox_dir/bazel-bin/engine/src/kernel/network_kernel.dll"
  staged_dylib="$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib"
  staged_dll="$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/network_kernel.dll"
  make_fake_repo "$repo_dir" "feat-unity-plugin"
  mkdir -p "$(dirname "$built_dylib")"
  echo "fake existing build output" > "$built_dylib"
  echo "fake existing windows build output" > "$built_dll"

  set +e
  run_builder "$repo_dir" "$output_file" --mode stage --unity off --auto-commit off
  status="$?"
  set -e

  [[ "$status" -eq 0 ]] || fail "expected stage mode to succeed with existing build output; got status $status: $(cat "$output_file")"
  [[ "$(cat "$staged_dylib")" == "fake existing build output" ]] || fail "expected stage mode to copy the existing build output"
  [[ "$(cat "$staged_dll")" == "fake existing windows build output" ]] || fail "expected stage mode to copy the existing Windows build output"
  for dll_name in libcrypto-4-x64.dll libssl-4-x64.dll libstdc++-6.dll libwinpthread-1.dll; do
    [[ -f "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/$dll_name" ]] ||
      fail "expected stage mode to copy $dll_name"
  done
  [[ -f "$codesign_log" ]] || fail "expected stage mode to verify the staged dylib signature"
  assert_contains "$(cat "$codesign_log")" "--verify --verbose=4 $staged_dylib"
  [[ -f "$xattr_log" ]] || fail "expected stage mode to clear quarantine from the staged dylib"
  assert_contains "$(cat "$xattr_log")" "-d com.apple.quarantine $staged_dylib"
}

test_verify_fails_when_windows_runtime_dll_is_missing() {
  local sandbox_dir repo_dir output_file status
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-missing-windows.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  make_fake_repo "$repo_dir" "feat-unity-plugin"
  rm "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/libwinpthread-1.dll"

  set +e
  run_builder "$repo_dir" "$output_file" --mode verify --unity off --auto-commit off
  status="$?"
  set -e

  [[ "$status" -ne 0 ]] || fail "expected verify to fail when a Windows support DLL is missing"
  assert_contains "$(cat "$output_file")" "libwinpthread-1.dll"
}

test_pack_removes_ds_store_files_from_package() {
  local sandbox_dir repo_dir output_file status
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-ds-store.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  make_fake_repo "$repo_dir" "feat-unity-plugin"
  echo "finder junk" > "$repo_dir/plugins/com.network-example.kernel/.DS_Store"
  echo "finder junk" > "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/.DS_Store"

  set +e
  run_builder "$repo_dir" "$output_file" --mode pack --unity off --auto-commit off
  status="$?"
  set -e

  [[ "$status" -eq 0 ]] || fail "expected pack mode to succeed; got status $status: $(cat "$output_file")"
  [[ ! -e "$repo_dir/plugins/com.network-example.kernel/.DS_Store" ]] || fail "expected root .DS_Store to be removed"
  [[ ! -e "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/Windows/x86_64/.DS_Store" ]] ||
    fail "expected nested .DS_Store to be removed"
}

test_requires_feat_unity_plugin_branch
test_release_notes_are_prepended_and_auto_committed_for_allowed_files
test_auto_commit_skips_when_disallowed_files_are_dirty
test_auto_commit_skips_without_release_note_bullets
test_build_native_verifies_bazel_signed_dylib
test_stage_verifies_staged_dylib_from_existing_build_output
test_verify_fails_when_windows_runtime_dll_is_missing
test_pack_removes_ds_store_files_from_package

echo "package_builder_finalize_test.sh: PASS"
