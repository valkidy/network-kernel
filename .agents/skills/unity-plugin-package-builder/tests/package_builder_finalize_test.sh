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
echo "fake bazel should not be called in verify mode" >&2
exit 1
SH

  write_file "$bin_dir/codesign" <<'SH'
#!/usr/bin/env bash
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
00000000 T _GameServer_Destroy
00000000 T _GameServer_HandleEvent
00000000 T _GameServer_Tick
00000000 T _GameServer_GetEnemyCount
00000000 T _GameServer_DespawnAll
SYMBOLS
SH

  for command_name in npm rsync tar xattr; do
    write_file "$bin_dir/$command_name" <<'SH'
#!/usr/bin/env bash
exit 0
SH
  done

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
    "$repo_dir/plugins/com.network-example.kernel/Editor" \
    "$repo_dir/plugins/com.network-example.kernel/Runtime/Core" \
    "$repo_dir/plugins/com.network-example.kernel/Runtime/Client" \
    "$repo_dir/plugins/com.network-example.kernel/Runtime/Host" \
    "$repo_dir/plugins/com.network-example.kernel/Samples~" \
    "$repo_dir/plugins/com.network-example.kernel/Tests~/AbiSmoke" \
    "$repo_dir/engine/src/kernel/public" \
    "$repo_dir/game_server/public"

  echo "fake dylib" > "$repo_dir/plugins/com.network-example.kernel/Assets/Plugins/macOS/libnetwork_kernel.dylib"
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

  for cs_file in \
    Runtime/Core/KernelNative.cs \
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
  mkdir -p "$fake_openssl"
  touch "$fake_openssl/libssl.3.dylib" "$fake_openssl/libcrypto.3.dylib"
  make_fake_command_dir "$fake_bin"

  (
    cd "$repo_dir"
    PATH="$fake_bin:$PATH" OPENSSL_LIB_DIR="$fake_openssl" "$SCRIPT_UNDER_TEST" "$@"
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
  local sandbox_dir repo_dir output_file notes_file subject
  sandbox_dir="$(mktemp -d "${TMPDIR:-/tmp}/package-builder-commit.XXXXXX")"
  repo_dir="$sandbox_dir/repo"
  output_file="$sandbox_dir/output.txt"
  notes_file="$repo_dir/plugins/com.network-example.kernel/RELEASE_NOTES.md"
  make_fake_repo "$repo_dir" "feat-unity-plugin"

  write_file "$notes_file" <<'MD'
0.6.3 release notes:

- previous fix
MD
  git -C "$repo_dir" add "$notes_file"
  git -C "$repo_dir" commit -q -m "docs: add previous release notes"

  echo "// allowed managed change" >> "$repo_dir/plugins/com.network-example.kernel/Runtime/Client/NetworkClient.cs"
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

test_requires_feat_unity_plugin_branch
test_release_notes_are_prepended_and_auto_committed_for_allowed_files
test_auto_commit_skips_when_disallowed_files_are_dirty
test_auto_commit_skips_without_release_note_bullets

echo "package_builder_finalize_test.sh: PASS"
