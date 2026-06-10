#!/usr/bin/env bash
set -euo pipefail

MODE="all"
PLATFORM="all"
UNITY_SETTING="auto"
OUTPUT_DIR=""
AUTO_COMMIT="on"
WORK_ROOT="/private/tmp/network-example-unity-package-builder"
OUTPUT_BASE="${OUTPUT_BASE:-/private/tmp/bazel-network-example}"
UNITY_TIMEOUT_SECONDS="${UNITY_TIMEOUT_SECONDS:-180}"
OPENSSL_LIB_DIR="${OPENSSL_LIB_DIR:-/opt/homebrew/opt/openssl@3/lib}"
OPENSSL_MINGW_DLL_DIR="${OPENSSL_MINGW_DLL_DIR:-}"
MINGW_RUNTIME_DLL_DIR="${MINGW_RUNTIME_DLL_DIR:-}"
MINGW_ROOT="${MINGW_ROOT:-/opt/homebrew/opt/mingw-w64/toolchain-x86_64}"

PACKAGE_DIR_REL="plugins/com.network-example.kernel"
PACKAGE_NAME="com.network-example.kernel"
NATIVE_TARGET="//engine/src/kernel:network_kernel_shared"
BUILT_MACOS_DYLIB_SUBPATH="engine/src/kernel/signed/libnetwork_kernel.dylib"
BUILT_WINDOWS_DLL_SUBPATH="engine/src/kernel/network_kernel.dll"
STAGED_MACOS_DYLIB_REL="${PACKAGE_DIR_REL}/Assets/Plugins/macOS/libnetwork_kernel.dylib"
STAGED_WINDOWS_DIR_REL="${PACKAGE_DIR_REL}/Assets/Plugins/Windows/x86_64"
STAGED_WINDOWS_DLL_REL="${STAGED_WINDOWS_DIR_REL}/network_kernel.dll"
RELEASE_NOTES_REL="${PACKAGE_DIR_REL}/RELEASE_NOTES.md"
RELEASE_NOTES_META_REL="${RELEASE_NOTES_REL}.meta"
RELEASE_NOTES=()
OPENSSL_WINDOWS_DLLS=(libcrypto-4-x64.dll libssl-4-x64.dll)
MINGW_RUNTIME_DLLS=(libstdc++-6.dll libwinpthread-1.dll)

REQUIRED_EXPORTS=(
  Kernel_Create
  Kernel_Destroy
  Kernel_GetAbiInfo
  Kernel_GetBuildInfo
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
  Kernel_ServerSubmitEntityInput
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
  --platform all|macos|windows-x86_64
  --unity auto|off|/absolute/path/to/Unity
  --output-dir /absolute/path
  --release-note "bullet text"    May be repeated; prepended to RELEASE_NOTES.md.
  --auto-commit on|off            Commit eligible package changes after success.
  -h, --help

Environment:
  UNITY_PACKAGE_BUILDER_ALLOW_INTEGRATION_BRANCH=1
      Allow integration/* branches for test-package validation. Requires
      --auto-commit off and does not relax the default feat-unity-plugin guard.
  GITHUB_ACTIONS=true GITHUB_REF_TYPE=branch GITHUB_REF_NAME=dev-latest
      Allow CI dev-package validation from dev-latest. Requires --auto-commit off.
  GITHUB_ACTIONS=true GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v*
      Allow CI release-package validation from immutable release tags. Requires
      --auto-commit off.

Default:
  Build the macOS and Windows x86_64 native plugins, stage them into the Unity
  package, verify ABI and exports, create a clean UPM .tgz under
  plugins/output, and run optional Unity batchmode smoke.
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
    --release-note)
      [[ "$#" -ge 2 ]] || die "--release-note requires a value"
      RELEASE_NOTES+=("$2")
      shift 2
      ;;
    --auto-commit)
      [[ "$#" -ge 2 ]] || die "--auto-commit requires a value"
      AUTO_COMMIT="$2"
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
  all|macos|windows-x86_64) ;;
  *) die "unsupported platform '$PLATFORM'. Use all, macos, or windows-x86_64." ;;
esac

case "$AUTO_COMMIT" in
  on|off) ;;
  *) die "unsupported --auto-commit '$AUTO_COMMIT'. Use on or off." ;;
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

platform_enabled() {
  local requested="$1"
  [[ "$PLATFORM" == "all" || "$PLATFORM" == "$requested" ]]
}

repo_root="$(find_repo_root)" || die "run this script from the network-example repo or one of its subdirectories"
cd "$repo_root"

require_branch() {
  local current_branch
  current_branch="$(git branch --show-current 2>/dev/null || true)"
  if [[ "$current_branch" == "feat-unity-plugin" ]]; then
    return 0
  fi
  if [[ "$AUTO_COMMIT" == "off" &&
        "${GITHUB_ACTIONS:-}" == "true" &&
        "${GITHUB_REF_TYPE:-}" == "branch" &&
        "${GITHUB_REF_NAME:-}" == "dev-latest" ]]; then
    note "CI dev branch override enabled: ${GITHUB_REF_NAME}"
    return 0
  fi
  if [[ "$AUTO_COMMIT" == "off" &&
        "${GITHUB_ACTIONS:-}" == "true" &&
        "${GITHUB_REF_TYPE:-}" == "tag" &&
        "${GITHUB_REF_NAME:-}" == v* ]]; then
    note "CI release tag override enabled: ${GITHUB_REF_NAME}"
    return 0
  fi
  if [[ "${UNITY_PACKAGE_BUILDER_ALLOW_INTEGRATION_BRANCH:-}" == "1" &&
        "$current_branch" == integration/* ]]; then
    [[ "$AUTO_COMMIT" == "off" ]] ||
      die "integration branch package validation requires --auto-commit off"
    note "Integration branch override enabled: $current_branch"
    return 0
  fi
  die "unity package builder must run on branch feat-unity-plugin, CI dev-latest, or CI v* tag with --auto-commit off; current branch is '${current_branch:-detached or unknown}'"
}

require_branch

PACKAGE_DIR="$repo_root/$PACKAGE_DIR_REL"
BUILT_MACOS_DYLIB=""
BUILT_WINDOWS_DLL=""
STAGED_MACOS_DYLIB="$repo_root/$STAGED_MACOS_DYLIB_REL"
STAGED_WINDOWS_DIR="$repo_root/$STAGED_WINDOWS_DIR_REL"
STAGED_WINDOWS_DLL="$repo_root/$STAGED_WINDOWS_DLL_REL"
RELEASE_NOTES_PATH="$repo_root/$RELEASE_NOTES_REL"

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
  require_command rsync
  require_command tar
  if platform_enabled macos; then
    require_command codesign
    require_command nm
    require_command xattr
  fi
  if platform_enabled windows-x86_64; then
    require_command file
    require_command unzip
    require_command x86_64-w64-mingw32-objdump
  fi
  mkdir -p "$OUTPUT_DIR" "$WORK_ROOT"
  [[ -w "$OUTPUT_DIR" ]] || die "output dir is not writable: $OUTPUT_DIR"
  [[ -w "$WORK_ROOT" ]] || die "work dir is not writable: $WORK_ROOT"
  if platform_enabled macos; then
    [[ -d "$OPENSSL_LIB_DIR" ]] || die "missing Homebrew OpenSSL library dir: $OPENSSL_LIB_DIR"
    [[ -f "$OPENSSL_LIB_DIR/libssl.3.dylib" ]] || die "missing $OPENSSL_LIB_DIR/libssl.3.dylib"
    [[ -f "$OPENSSL_LIB_DIR/libcrypto.3.dylib" ]] || die "missing $OPENSSL_LIB_DIR/libcrypto.3.dylib"
  fi
}

read_package_version() {
  node -e "const p=require(process.argv[1]); console.log(p.version || '')" "$PACKAGE_DIR/package.json"
}

verify_signed_native_plugin() {
  local dylib_path="$1"
  local description="$2"
  [[ -f "$dylib_path" ]] || die "native plugin not found for codesign verification: $dylib_path"
  note "Verifying ad-hoc signature for $description"
  codesign --verify --verbose=4 "$dylib_path"
}

clear_quarantine_xattr() {
  local dylib_path="$1"
  if xattr -p com.apple.quarantine "$dylib_path" >/dev/null 2>&1; then
    xattr -d com.apple.quarantine "$dylib_path" ||
      die "could not remove com.apple.quarantine from $dylib_path"
  fi
}

resolve_bazel_bin() {
  local config="$1"
  "$BAZEL_CMD" --output_base="$OUTPUT_BASE" info "--config=$config" -c opt bazel-bin --symlink_prefix=/
}

resolve_built_macos_dylib() {
  local bazel_bin
  bazel_bin="$(resolve_bazel_bin macos)"
  BUILT_MACOS_DYLIB="$bazel_bin/$BUILT_MACOS_DYLIB_SUBPATH"
  [[ -f "$BUILT_MACOS_DYLIB" ]] || die "expected built macOS dylib not found: $BUILT_MACOS_DYLIB"
}

resolve_built_windows_dll() {
  local bazel_bin
  bazel_bin="$(resolve_bazel_bin mingw_w64)"
  BUILT_WINDOWS_DLL="$bazel_bin/$BUILT_WINDOWS_DLL_SUBPATH"
  [[ -f "$BUILT_WINDOWS_DLL" ]] || die "expected built Windows DLL not found: $BUILT_WINDOWS_DLL"
}

build_native() {
  if platform_enabled macos; then
    note "Building macOS native plugin: $NATIVE_TARGET"
    "$BAZEL_CMD" \
      --output_base="$OUTPUT_BASE" \
      build \
      --config=macos \
      --symlink_prefix=/ \
      --copt=-Wunused-function \
      -c opt \
      "$NATIVE_TARGET"
    resolve_built_macos_dylib
    verify_signed_native_plugin "$BUILT_MACOS_DYLIB" "Bazel-built macOS native plugin"
  fi

  if platform_enabled windows-x86_64; then
    note "Building Windows x86_64 native plugin: $NATIVE_TARGET"
    "$BAZEL_CMD" \
      --output_base="$OUTPUT_BASE" \
      build \
      --config=mingw_w64 \
      --symlink_prefix=/ \
      -c opt \
      "$NATIVE_TARGET"
    resolve_built_windows_dll
  fi
}

copy_existing_windows_dll() {
  local dll_name="$1"
  shift
  local source_dir
  for source_dir in "$@"; do
    [[ -n "$source_dir" ]] || continue
    if [[ -f "$source_dir/$dll_name" ]]; then
      copy_overwriting "$source_dir/$dll_name" "$STAGED_WINDOWS_DIR/$dll_name"
      return 0
    fi
  done
  return 1
}

copy_overwriting() {
  local source_path="$1"
  local destination_path="$2"
  if [[ -e "$destination_path" ]]; then
    chmod u+w "$destination_path" 2>/dev/null || true
  fi
  cp "$source_path" "$destination_path"
}

copy_openssl_windows_dll() {
  local dll_name="$1"
  if copy_existing_windows_dll "$dll_name" "$OPENSSL_MINGW_DLL_DIR"; then
    return 0
  fi

  local archive_path="$repo_root/third_party/openssl_mingw-4.0.0.zip"
  [[ -f "$archive_path" ]] || die "missing OpenSSL MinGW archive: $archive_path"
  local destination_path="$STAGED_WINDOWS_DIR/$dll_name"
  if [[ -e "$destination_path" ]]; then
    chmod u+w "$destination_path" 2>/dev/null || true
  fi
  unzip -p "$archive_path" "openssl_x86_64/lib/$dll_name" > "$destination_path" ||
    die "could not extract $dll_name from $archive_path"
}

copy_mingw_runtime_dll() {
  local dll_name="$1"
  if copy_existing_windows_dll \
      "$dll_name" \
      "$MINGW_RUNTIME_DLL_DIR" \
      "$MINGW_ROOT/x86_64-w64-mingw32/bin" \
      "$MINGW_ROOT/x86_64-w64-mingw32/lib"; then
    return 0
  fi
  die "missing MinGW runtime DLL: $dll_name"
}

stage_native() {
  if platform_enabled macos; then
    if [[ -z "$BUILT_MACOS_DYLIB" ]]; then
      resolve_built_macos_dylib
    fi
    [[ -f "$BUILT_MACOS_DYLIB" ]] || die "built macOS dylib not found: $BUILT_MACOS_DYLIB. Run --mode build-native or --mode all first."
    mkdir -p "$(dirname "$STAGED_MACOS_DYLIB")"
    cp "$BUILT_MACOS_DYLIB" "$STAGED_MACOS_DYLIB"
    clear_quarantine_xattr "$STAGED_MACOS_DYLIB"
    verify_signed_native_plugin "$STAGED_MACOS_DYLIB" "staged macOS native plugin"
    note "Staged macOS native plugin: $STAGED_MACOS_DYLIB"
  fi

  if platform_enabled windows-x86_64; then
    if [[ -z "$BUILT_WINDOWS_DLL" ]]; then
      resolve_built_windows_dll
    fi
    [[ -f "$BUILT_WINDOWS_DLL" ]] || die "built Windows DLL not found: $BUILT_WINDOWS_DLL. Run --mode build-native or --mode all first."
    mkdir -p "$STAGED_WINDOWS_DIR"
    copy_overwriting "$BUILT_WINDOWS_DLL" "$STAGED_WINDOWS_DLL"
    local dll_name
    for dll_name in "${OPENSSL_WINDOWS_DLLS[@]}"; do
      copy_openssl_windows_dll "$dll_name"
    done
    for dll_name in "${MINGW_RUNTIME_DLLS[@]}"; do
      copy_mingw_runtime_dll "$dll_name"
    done
    note "Staged Windows x86_64 native plugin: $STAGED_WINDOWS_DLL"
  fi
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

build_required_exports() {
  local native_abi="$1"
  local native_game_server_abi="$2"
  REQUIRED_EXPORTS_BUILT=("${REQUIRED_EXPORTS[@]}")
  if [[ "$native_abi" -ge 9 ]]; then
    REQUIRED_EXPORTS_BUILT+=(
      Kernel_ServerSetEntityCombatState
      Kernel_ServerSetEntityWeaponMechanics
      Kernel_ServerClearEntityWeaponMechanics
      Kernel_ServerGetEntityWeaponMechanics
      Kernel_ServerValidateMechanicsConfig
    )
  fi
  if [[ "$native_abi" -ge 10 ]]; then
    REQUIRED_EXPORTS_BUILT+=(Kernel_ServerGetAreaEffectState)
  fi
  if [[ "$native_abi" -ge 11 ]]; then
    REQUIRED_EXPORTS_BUILT+=(Kernel_ServerGetBeamState)
  fi
  if [[ "$native_abi" -ge 12 ]]; then
    REQUIRED_EXPORTS_BUILT+=(Kernel_ServerGetHomingState)
  fi
  if [[ "$native_abi" -ge 14 ]]; then
    REQUIRED_EXPORTS_BUILT+=(
      Kernel_LANDiscovery_Create
      Kernel_LANDiscovery_Destroy
      Kernel_LANDiscovery_StartServer
      Kernel_LANDiscovery_StopServer
      Kernel_LANDiscovery_Query
      Kernel_LANDiscovery_PollResults
      Kernel_LANDiscovery_ClearResults
    )
  fi
  if [[ "$native_abi" -ge 15 ]]; then
    REQUIRED_EXPORTS_BUILT+=(
      Kernel_GetBenchmarkStats
      Kernel_GetNetworkStats
      Kernel_PollDebugRecords
      Kernel_QueryColliderShapes
    )
  fi
  if [[ "$native_abi" -ge 16 ]]; then
    REQUIRED_EXPORTS_BUILT+=(
      Kernel_LoadGameplayCatalogFromMemory
    )
  fi
  if [[ "$native_game_server_abi" -ge 2 ]]; then
    REQUIRED_EXPORTS_BUILT+=(
      GameServer_CreateWithWeaponTemplateDirectory
      GameServer_QueryWeaponTemplate
    )
  fi
  if [[ "$native_game_server_abi" -ge 3 ]]; then
    REQUIRED_EXPORTS_BUILT+=(
      GameServer_CreateWithGameplayCatalogFromMemory
    )
  fi
}

verify_macos_exports() {
  local dylib_path="$1"
  shift
  local required_exports=("$@")
  local exported symbol
  exported="$(nm -gU "$dylib_path")"
  for symbol in "${required_exports[@]}"; do
    if ! grep -q "_${symbol}$" <<<"$exported"; then
      die "missing macOS exported symbol: $symbol"
    fi
  done
}

verify_windows_exports() {
  local dll_path="$1"
  shift
  local required_exports=("$@")
  local file_output exported symbol
  file_output="$(file "$dll_path")"
  [[ "$file_output" == *"PE32+"* && "$file_output" == *"x86-64"* ]] ||
    die "Windows native plugin is not a PE32+ x86-64 DLL: $dll_path"
  exported="$(x86_64-w64-mingw32-objdump -p "$dll_path")"
  for symbol in "${required_exports[@]}"; do
    if ! grep -Eq "[[:space:]]${symbol}$" <<<"$exported"; then
      die "missing Windows exported symbol: $symbol"
    fi
  done
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
  grep -q 'LibraryName = "network_kernel"' "$PACKAGE_DIR/Runtime/Core/KernelNative.cs" ||
    die "KernelNative.LibraryName must be network_kernel"
  if platform_enabled macos; then
    [[ -f "$STAGED_MACOS_DYLIB" ]] || die "missing staged macOS dylib: $STAGED_MACOS_DYLIB"
  fi
  if platform_enabled windows-x86_64; then
    [[ -f "$STAGED_WINDOWS_DLL" ]] || die "missing staged Windows DLL: $STAGED_WINDOWS_DLL"
    local dll_name
    for dll_name in "${OPENSSL_WINDOWS_DLLS[@]}" "${MINGW_RUNTIME_DLLS[@]}"; do
      [[ -f "$STAGED_WINDOWS_DIR/$dll_name" ]] || die "missing staged Windows support DLL: $STAGED_WINDOWS_DIR/$dll_name"
    done
  fi

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

  local required_exports=()
  build_required_exports "$native_abi" "$native_game_server_abi"
  required_exports=("${REQUIRED_EXPORTS_BUILT[@]}")
  if platform_enabled macos; then
    verify_macos_exports "$STAGED_MACOS_DYLIB" "${required_exports[@]}"
  fi
  if platform_enabled windows-x86_64; then
    verify_windows_exports "$STAGED_WINDOWS_DLL" "${required_exports[@]}"
  fi

  note "Verification passed: package=$PACKAGE_NAME kernel_abi=$native_abi game_server_abi=$native_game_server_abi exports=${#required_exports[@]}"
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

remove_package_ds_store() {
  note "Removing .DS_Store files from package"
  find "$PACKAGE_DIR" -name .DS_Store -type f -delete
}

pack_package() {
  remove_package_ds_store
  verify_package
  local version staging_dir npm_output tgz_name tgz_path
  version="$(read_package_version)"
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

write_release_notes() {
  local version="$1"
  local temp_path
  mkdir -p "$(dirname "$RELEASE_NOTES_PATH")"
  temp_path="$RELEASE_NOTES_PATH.tmp.$$"
  {
    printf '%s release notes:\n\n' "$version"
    local release_note
    for release_note in "${RELEASE_NOTES[@]}"; do
      printf -- '- %s\n' "$release_note"
    done
    if [[ -s "$RELEASE_NOTES_PATH" ]]; then
      printf '\n\n'
      cat "$RELEASE_NOTES_PATH"
    fi
  } > "$temp_path"
  mv "$temp_path" "$RELEASE_NOTES_PATH"
  note "Updated release notes: $RELEASE_NOTES_PATH"
}

is_auto_commit_allowed_path() {
  local path="$1"
  if [[ "$path" == "$RELEASE_NOTES_REL" || "$path" == "$RELEASE_NOTES_META_REL" ]]; then
    return 0
  fi
  if [[ "$path" == "$PACKAGE_DIR_REL/Assets/Plugins/"* ]]; then
    case "$path" in
      *.dll|*.dylib|*.meta) return 0 ;;
    esac
  fi
  if [[ "$path" == "$PACKAGE_DIR_REL/"* && "$path" == *.cs ]]; then
    return 0
  fi
  return 1
}

finalize_auto_commit() {
  if [[ "$AUTO_COMMIT" == "off" ]]; then
    note "Auto commit skipped: disabled by --auto-commit off"
    return 0
  fi

  local version
  version="$(read_package_version)"
  [[ -n "$version" ]] || die "package.json is missing version"

  if [[ "${#RELEASE_NOTES[@]}" -gt 0 ]]; then
    write_release_notes "$version"
  fi

  local status_output
  status_output="$(git status --porcelain=v1 --untracked-files=all)"
  if [[ -z "$status_output" ]]; then
    note "Auto commit skipped: no package changes detected"
    return 0
  fi

  if [[ "${#RELEASE_NOTES[@]}" -eq 0 ]]; then
    note "Auto commit skipped: provide at least one --release-note to create $RELEASE_NOTES_REL"
    return 0
  fi

  local disallowed_paths=()
  local allowed_paths=()
  local line path
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    path="${line:3}"
    if is_auto_commit_allowed_path "$path"; then
      allowed_paths+=("$path")
    else
      disallowed_paths+=("$path")
    fi
  done <<< "$status_output"

  if [[ "${#disallowed_paths[@]}" -gt 0 ]]; then
    note "Auto commit skipped: dirty files outside the Unity package release allowlist"
    printf '%s\n' "${disallowed_paths[@]}"
    return 0
  fi

  if [[ "${#allowed_paths[@]}" -eq 0 ]]; then
    note "Auto commit skipped: no eligible package files changed"
    return 0
  fi

  git add -- "${allowed_paths[@]}"
  if git diff --cached --quiet; then
    note "Auto commit skipped: no staged package changes"
    return 0
  fi

  git commit -m "feat: bump Unity package to $version"
  note "Auto commit created: feat: bump Unity package to $version"
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

finalize_auto_commit

note "Summary"
if [[ -f "$STAGED_MACOS_DYLIB" ]]; then
  echo "staged_macos_dylib=$STAGED_MACOS_DYLIB"
fi
if [[ -f "$STAGED_WINDOWS_DLL" ]]; then
  echo "staged_windows_dll=$STAGED_WINDOWS_DLL"
fi
if [[ -n "$ARTIFACT_PATH" ]]; then
  echo "artifact=$ARTIFACT_PATH"
fi
echo "unity=$UNITY_SETTING"
echo "mode=$MODE"
echo "platform=$PLATFORM"
