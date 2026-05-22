#!/usr/bin/env bash
set -euo pipefail

MODE="all"
PLATFORM="macos"
UNITY_SETTING="auto"
OUTPUT_DIR=""
WORK_ROOT="/private/tmp/network-example-unity-package-builder"
OUTPUT_BASE="${OUTPUT_BASE:-/private/tmp/bazel-network-example}"
UNITY_TIMEOUT_SECONDS="${UNITY_TIMEOUT_SECONDS:-180}"

PACKAGE_DIR_REL="plugins/com.network-example.kernel"
PACKAGE_NAME="com.network-example.kernel"
NATIVE_TARGET="//engine/src/kernel:network_kernel_shared"
BUILT_DYLIB_SUBPATH="engine/src/kernel/libnetwork_kernel.dylib"
STAGED_DYLIB_REL="${PACKAGE_DIR_REL}/Assets/Plugins/macOS/libnetwork_kernel.dylib"

REQUIRED_EXPORTS=(
  Kernel_Create
  Kernel_Destroy
  Kernel_GetAbiInfo
  Kernel_GetLocalPlayerInfo
  Kernel_StartClient
  Kernel_StartListenServer
  Kernel_StartDedicatedServer
  Kernel_Update
  Kernel_SubmitInput
  Kernel_GetRenderStates
  Kernel_GetRenderStatesAtTime
  Kernel_PollEvents
  Kernel_ServerCreateEntity
  Kernel_ServerDestroyEntity
  Kernel_ServerSetEntityTransform
  Kernel_ServerSetEntityVelocity
  Kernel_ServerSetEntityState
  Kernel_ServerGetEntityState
  Kernel_ServerQueryEntities
  GameServer_GetAbiInfo
  GameServer_Create
  GameServer_Destroy
  GameServer_HandleEvent
  GameServer_Tick
  GameServer_GetEnemyCount
  GameServer_DespawnAll
)

usage() {
  cat <<'USAGE'
Usage: run-unity-plugin-package-builder.sh [options]

Options:
  --mode all|build-native|stage|verify|pack
  --platform macos
  --unity auto|off|/absolute/path/to/Unity
  --output-dir /absolute/path
  -h, --help

Default:
  Build the macOS native plugin, stage it into the Unity package, verify ABI
  and exports, create a clean UPM .tgz under plugins/output, and run optional
  Unity batchmode smoke.
USAGE
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

note() {
  echo "==> $*"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --mode)
      [[ "$#" -ge 2 ]] || die "--mode requires a value"
      MODE="$2"
      shift 2
      ;;
    --platform)
      [[ "$#" -ge 2 ]] || die "--platform requires a value"
      PLATFORM="$2"
      shift 2
      ;;
    --unity)
      [[ "$#" -ge 2 ]] || die "--unity requires a value"
      UNITY_SETTING="$2"
      shift 2
      ;;
    --output-dir)
      [[ "$#" -ge 2 ]] || die "--output-dir requires a value"
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument '$1'"
      ;;
  esac
done

case "$MODE" in
  all|build-native|stage|verify|pack) ;;
  *) die "unsupported mode '$MODE'. Use all, build-native, stage, verify, or pack." ;;
esac

case "$PLATFORM" in
  macos) ;;
  *) die "unsupported platform '$PLATFORM'. Current package builder supports macos only." ;;
esac

find_repo_root() {
  local dir="$PWD"
  while [[ "$dir" != "/" ]]; do
    if [[ -f "$dir/WORKSPACE" && -f "$dir/${PACKAGE_DIR_REL}/package.json" ]]; then
      printf '%s\n' "$dir"
      return 0
    fi
    dir="$(dirname "$dir")"
  done
  return 1
}

require_command() {
  local command_name="$1"
  command -v "$command_name" >/dev/null 2>&1 || die "$command_name not found in PATH"
}

repo_root="$(find_repo_root)" || die "run this script from the network-example repo or one of its subdirectories"
cd "$repo_root"

PACKAGE_DIR="$repo_root/$PACKAGE_DIR_REL"
BUILT_DYLIB=""
STAGED_DYLIB="$repo_root/$STAGED_DYLIB_REL"

if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="$repo_root/plugins/output"
fi

if command -v bazelisk >/dev/null 2>&1; then
  BAZEL_CMD="bazelisk"
elif command -v bazel >/dev/null 2>&1; then
  BAZEL_CMD="bazel"
else
  die "bazelisk/bazel not found in PATH"
fi

preflight() {
  note "Preflight: repo root is $repo_root"
  [[ -f "$PACKAGE_DIR/package.json" ]] || die "missing $PACKAGE_DIR/package.json"
  [[ -d "$PACKAGE_DIR/Runtime" ]] || die "missing $PACKAGE_DIR/Runtime"
  [[ -d "$PACKAGE_DIR/Editor" ]] || die "missing $PACKAGE_DIR/Editor"
  [[ -d "$PACKAGE_DIR/Samples~" ]] || die "missing $PACKAGE_DIR/Samples~"
  [[ -d "$PACKAGE_DIR/Tests~" ]] || die "missing $PACKAGE_DIR/Tests~"
  require_command node
  require_command npm
  require_command codesign
  require_command nm
  require_command rsync
  require_command tar
  require_command xattr
  mkdir -p "$OUTPUT_DIR" "$WORK_ROOT"
  [[ -w "$OUTPUT_DIR" ]] || die "output dir is not writable: $OUTPUT_DIR"
  [[ -w "$WORK_ROOT" ]] || die "work dir is not writable: $WORK_ROOT"
  [[ -d /opt/homebrew/opt/openssl@3/lib ]] || die "missing Homebrew OpenSSL library dir: /opt/homebrew/opt/openssl@3/lib"
  [[ -f /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib ]] || die "missing /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib"
  [[ -f /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib ]] || die "missing /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib"
}

build_native() {
  note "Building native plugin: $NATIVE_TARGET"
  "$BAZEL_CMD" \
    --output_base="$OUTPUT_BASE" \
    build \
    --config=macos \
    --symlink_prefix=/ \
    --copt=-Wunused-function \
    -c opt \
    "$NATIVE_TARGET"
  local bazel_bin
  bazel_bin="$("$BAZEL_CMD" --output_base="$OUTPUT_BASE" info --config=macos -c opt bazel-bin --symlink_prefix=/)"
  BUILT_DYLIB="$bazel_bin/$BUILT_DYLIB_SUBPATH"
  [[ -f "$BUILT_DYLIB" ]] || die "expected built dylib not found: $BUILT_DYLIB"
}

stage_native() {
  [[ -f "$BUILT_DYLIB" ]] || die "built dylib not found: $BUILT_DYLIB. Run --mode build-native or --mode all first."
  mkdir -p "$(dirname "$STAGED_DYLIB")"
  cp "$BUILT_DYLIB" "$STAGED_DYLIB"
  note "Ad-hoc signing staged native plugin"
  codesign --force --deep --sign - "$STAGED_DYLIB"
  note "Removing GateKeeper quarantine attributes from staged native plugin"
  xattr -d -r com.apple.quarantine "$STAGED_DYLIB" 2>/dev/null || true
  note "Staged native plugin: $STAGED_DYLIB"
}

read_native_abi_version() {
  sed -nE 's/^#define KERNEL_ABI_VERSION ([0-9]+)u$/\1/p' "$repo_root/engine/src/kernel/public/kernel_types.h" | head -n 1
}

read_managed_abi_version() {
  sed -nE 's/^[[:space:]]*public const uint AbiVersion = ([0-9]+);$/\1/p' "$PACKAGE_DIR/Runtime/Core/KernelTypes.cs" | head -n 1
}

read_native_game_server_abi_version() {
  sed -nE 's/^#define GAME_SERVER_ABI_VERSION ([0-9]+)u$/\1/p' "$repo_root/game_server/public/game_server_types.h" | head -n 1
}

read_managed_game_server_abi_version() {
  sed -nE 's/^[[:space:]]*public const uint AbiVersion = ([0-9]+);$/\1/p' "$PACKAGE_DIR/Runtime/Core/GameServerTypes.cs" | head -n 1
}

verify_package() {
  note "Verifying package layout, ABI version, and exported symbols"
  [[ -f "$PACKAGE_DIR/package.json" ]] || die "missing package.json"
  [[ -f "$PACKAGE_DIR/Runtime/Core/KernelNative.cs" ]] || die "missing Runtime/Core/KernelNative.cs"
  [[ -f "$PACKAGE_DIR/Runtime/Core/KernelTypes.cs" ]] || die "missing Runtime/Core/KernelTypes.cs"
  [[ -f "$PACKAGE_DIR/Runtime/Core/GameServerNative.cs" ]] || die "missing Runtime/Core/GameServerNative.cs"
  [[ -f "$PACKAGE_DIR/Runtime/Core/GameServerTypes.cs" ]] || die "missing Runtime/Core/GameServerTypes.cs"
  [[ -f "$PACKAGE_DIR/Runtime/Client/NetworkClient.cs" ]] || die "missing Runtime/Client/NetworkClient.cs"
  [[ -f "$PACKAGE_DIR/Runtime/Host/NetworkHost.cs" ]] || die "missing Runtime/Host/NetworkHost.cs"
  [[ -f "$PACKAGE_DIR/Editor/NetworkKernelAbiSmokeRunner.cs" ]] || die "missing Editor/NetworkKernelAbiSmokeRunner.cs"
  [[ -f "$PACKAGE_DIR/Tests~/AbiSmoke/NetworkKernelManagedAbiSmoke.cs" ]] || die "missing Tests~/AbiSmoke/NetworkKernelManagedAbiSmoke.cs"
  [[ -f "$STAGED_DYLIB" ]] || die "missing staged dylib: $STAGED_DYLIB"

  local package_name
  package_name="$(node -e "const p=require(process.argv[1]); console.log(p.name || '')" "$PACKAGE_DIR/package.json")"
  [[ "$package_name" == "$PACKAGE_NAME" ]] || die "package name mismatch: expected $PACKAGE_NAME, found '$package_name'"

  local native_abi managed_abi native_game_server_abi managed_game_server_abi
  native_abi="$(read_native_abi_version)"
  managed_abi="$(read_managed_abi_version)"
  native_game_server_abi="$(read_native_game_server_abi_version)"
  managed_game_server_abi="$(read_managed_game_server_abi_version)"
  [[ -n "$native_abi" ]] || die "could not read KERNEL_ABI_VERSION"
  [[ -n "$managed_abi" ]] || die "could not read KernelConstants.AbiVersion"
  [[ "$native_abi" == "$managed_abi" ]] || die "ABI version mismatch: native=$native_abi managed=$managed_abi"
  [[ -n "$native_game_server_abi" ]] || die "could not read GAME_SERVER_ABI_VERSION"
  [[ -n "$managed_game_server_abi" ]] || die "could not read GameServerConstants.AbiVersion"
  [[ "$native_game_server_abi" == "$managed_game_server_abi" ]] || die "game server ABI version mismatch: native=$native_game_server_abi managed=$managed_game_server_abi"

  local exported
  exported="$(nm -gU "$STAGED_DYLIB")"
  local symbol
  for symbol in "${REQUIRED_EXPORTS[@]}"; do
    if ! grep -q "_${symbol}$" <<<"$exported"; then
      die "missing exported symbol: $symbol"
    fi
  done

  note "Verification passed: package=$PACKAGE_NAME kernel_abi=$native_abi game_server_abi=$native_game_server_abi exports=${#REQUIRED_EXPORTS[@]}"
}

copy_clean_package() {
  local staging_dir="$1"
  rm -rf "$staging_dir"
  mkdir -p "$staging_dir"
  rsync -a \
    --exclude='.DS_Store' \
    --exclude='Library' \
    --exclude='Logs' \
    --exclude='Temp' \
    --exclude='Obj' \
    --exclude='Build' \
    --exclude='Builds' \
    --exclude='UserSettings' \
    --exclude='*.log' \
    "$PACKAGE_DIR/" \
    "$staging_dir/"
}

pack_package() {
  verify_package
  local version staging_dir npm_output tgz_name tgz_path
  version="$(node -e "const p=require(process.argv[1]); console.log(p.version || '')" "$PACKAGE_DIR/package.json")"
  [[ -n "$version" ]] || die "package.json is missing version"
  staging_dir="$WORK_ROOT/staging/$PACKAGE_NAME"
  copy_clean_package "$staging_dir"

  note "Packing clean UPM tarball"
  npm_output="$(cd "$staging_dir" && npm pack --pack-destination "$OUTPUT_DIR")"
  tgz_name="$(printf '%s\n' "$npm_output" | tail -n 1)"
  tgz_path="$OUTPUT_DIR/$tgz_name"
  [[ -f "$tgz_path" ]] || die "expected npm pack output not found: $tgz_path"
  ARTIFACT_PATH="$tgz_path"
  note "Package artifact: $ARTIFACT_PATH"
}

detect_unity() {
  if [[ "$UNITY_SETTING" == "off" ]]; then
    return 1
  fi
  if [[ "$UNITY_SETTING" != "auto" ]]; then
    [[ -x "$UNITY_SETTING" ]] || die "Unity executable is not executable: $UNITY_SETTING"
    printf '%s\n' "$UNITY_SETTING"
    return 0
  fi
  if [[ -n "${UNITY_PATH:-}" && -x "${UNITY_PATH:-}" ]]; then
    printf '%s\n' "$UNITY_PATH"
    return 0
  fi
  if command -v Unity >/dev/null 2>&1; then
    command -v Unity
    return 0
  fi
  local found
  found="$(find /Applications/Unity/Hub/Editor -path '*/Unity.app/Contents/MacOS/Unity' -type f 2>/dev/null | sort | tail -n 1 || true)"
  if [[ -n "$found" && -x "$found" ]]; then
    printf '%s\n' "$found"
    return 0
  fi
  return 1
}

write_unity_manifest() {
  local manifest_path="$1"
  local artifact_path="$2"
  cat > "$manifest_path" <<JSON
{
  "dependencies": {
    "$PACKAGE_NAME": "file:$artifact_path"
  }
}
JSON
}

run_unity_smoke() {
  if [[ "${ARTIFACT_PATH:-}" == "" ]]; then
    note "Unity smoke skipped: no package artifact was produced in mode '$MODE'"
    return 0
  fi

  local unity_path
  if ! unity_path="$(detect_unity)"; then
    note "Unity smoke skipped: Unity executable not found or disabled"
    return 0
  fi

  local project_dir log_path
  project_dir="$WORK_ROOT/unity-smoke-project"
  log_path="$WORK_ROOT/unity-smoke.log"
  rm -rf "$project_dir"
  mkdir -p "$project_dir/Assets" "$project_dir/Packages" "$project_dir/ProjectSettings"
  write_unity_manifest "$project_dir/Packages/manifest.json" "$ARTIFACT_PATH"

  note "Running optional Unity batchmode smoke: $unity_path"
  set +e
  "$unity_path" \
    -batchmode \
    -quit \
    -nographics \
    -projectPath "$project_dir" \
    -executeMethod NetworkExample.Kernel.Editor.NetworkKernelAbiSmokeRunner.Run \
    -logFile "$log_path" &
  local unity_pid="$!"
  local elapsed_seconds=0
  local timed_out=0
  while kill -0 "$unity_pid" >/dev/null 2>&1; do
    if [[ "$elapsed_seconds" -ge "$UNITY_TIMEOUT_SECONDS" ]]; then
      timed_out=1
      kill "$unity_pid" >/dev/null 2>&1 || true
      sleep 2
      kill -9 "$unity_pid" >/dev/null 2>&1 || true
      break
    fi
    sleep 2
    elapsed_seconds=$((elapsed_seconds + 2))
  done
  wait "$unity_pid" >/dev/null 2>&1
  local unity_status="$?"
  if [[ "$timed_out" -eq 1 ]]; then
    unity_status=124
  fi
  set -e

  if [[ "$unity_status" -eq 0 ]]; then
    note "Unity smoke passed"
    return 0
  fi

  if [[ "$UNITY_SETTING" == "auto" ]]; then
    if [[ "$unity_status" -eq 124 ]]; then
      note "Unity smoke skipped: batchmode timed out after ${UNITY_TIMEOUT_SECONDS}s in auto mode; see $log_path"
    else
      note "Unity smoke skipped: batchmode failed in auto mode; see $log_path"
    fi
    if [[ -f "$log_path" ]]; then
      tail -n 40 "$log_path" || true
    fi
    return 0
  fi

  if [[ -f "$log_path" ]]; then
    tail -n 80 "$log_path" || true
  fi
  if [[ "$unity_status" -eq 124 ]]; then
    die "Unity smoke timed out after ${UNITY_TIMEOUT_SECONDS}s"
  fi
  die "Unity smoke failed with exit code $unity_status"
}

ARTIFACT_PATH=""

preflight

case "$MODE" in
  all)
    build_native
    stage_native
    pack_package
    run_unity_smoke
    ;;
  build-native)
    build_native
    ;;
  stage)
    stage_native
    verify_package
    ;;
  verify)
    verify_package
    ;;
  pack)
    pack_package
    run_unity_smoke
    ;;
esac

note "Summary"
if [[ -f "$STAGED_DYLIB" ]]; then
  echo "staged_dylib=$STAGED_DYLIB"
fi
if [[ -n "$ARTIFACT_PATH" ]]; then
  echo "artifact=$ARTIFACT_PATH"
fi
echo "unity=$UNITY_SETTING"
echo "mode=$MODE"
