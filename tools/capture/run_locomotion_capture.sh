#!/usr/bin/env bash
#
# Orchestrates the locomotion capture: preflight -> native run -> plugin run ->
# mp4 render -> parity report. Invoked through
# //engine/src/tests/kernel_tests:locomotion_capture, which passes every tool
# and asset path; see that target's BUILD comment for the user-facing usage.

set -euo pipefail

# --- Tool and asset paths (filled in by the sh_binary's args attribute). ---
driver=""
dylib=""
bundle=""
manifest=""
preflight=""
render=""
analyze=""

# --- Capture parameters (overridable after `--` on the bazel run command). ---
sampling="300"
tick_rate="30"
path="+X"
spawn="0,10,0"
entry="monster_observer_gameplay_catalog.yaml"
entity_template="20"
port="7777"
skip_plugin="0"

for argument in "$@"; do
  case "${argument}" in
    --driver=*) driver="${argument#*=}" ;;
    --dylib=*) dylib="${argument#*=}" ;;
    --bundle=*) bundle="${argument#*=}" ;;
    --manifest=*) manifest="${argument#*=}" ;;
    --preflight=*) preflight="${argument#*=}" ;;
    --render=*) render="${argument#*=}" ;;
    --analyze=*) analyze="${argument#*=}" ;;
    --sampling=*) sampling="${argument#*=}" ;;
    --samples=*) sampling="${argument#*=}" ;;
    --tick-rate=*) tick_rate="${argument#*=}" ;;
    --path=*) path="${argument#*=}" ;;
    --spawn=*) spawn="${argument#*=}" ;;
    --entry=*) entry="${argument#*=}" ;;
    --entity-template=*) entity_template="${argument#*=}" ;;
    --port=*) port="${argument#*=}" ;;
    --skip-plugin) skip_plugin="1" ;;
    -h | --help)
      printf 'usage: bazel run //engine/src/tests/kernel_tests:locomotion_capture -- \\\n'
      printf '         [--sampling=300] [--tick-rate=30] [--path="+X"] \\\n'
      printf '         [--spawn=0,10,0] [--skip-plugin]\n'
      exit 0
      ;;
    *)
      printf 'error: unknown argument %s\n' "${argument}" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
  printf 'error: run this through `bazel run`, not by executing the binary directly\n' >&2
  exit 2
fi

absolute() {
  case "$1" in
    /*) printf '%s' "$1" ;;
    *) printf '%s/%s' "$(pwd)" "$1" ;;
  esac
}

driver="$(absolute "${driver}")"
dylib="$(absolute "${dylib}")"
bundle="$(absolute "${bundle}")"
manifest="$(absolute "${manifest}")"
preflight="$(absolute "${preflight}")"
render="$(absolute "${render}")"
analyze="$(absolute "${analyze}")"

for tool in "${driver}" "${dylib}" "${bundle}" "${manifest}" "${preflight}" \
  "${render}" "${analyze}"; do
  if [[ ! -e "${tool}" ]]; then
    printf 'error: missing input %s\n' "${tool}" >&2
    exit 1
  fi
done

readonly workspace="${BUILD_WORKSPACE_DIRECTORY}"
readonly output_dir="${workspace}/capture/locomotion_tests"
mkdir -p "${output_dir}"

printf '=== locomotion capture ===\n'
printf 'output:   %s\n' "${output_dir}"
printf 'sampling: %s samples @ %s Hz\n' "${sampling}" "${tick_rate}"
printf 'path:     %s\n' "${path}"

# --- 1. Environment preflight: stop before any expensive work. -------------
printf '\n--- preflight ---\n'
if ! "${preflight}" \
  --output-dir="${output_dir}" \
  --error-log="${output_dir}/preflight_error.log"; then
  printf '\nerror: capture aborted, the Python tooling cannot run here.\n' >&2
  printf 'see %s/preflight_error.log\n' "${output_dir}" >&2
  exit 1
fi
rm -f "${output_dir}/preflight_error.log"

# --- 2. Stage the freshly built kernel into the Unity package. -------------
readonly plugin_root="${workspace}/plugins/com.network-example.kernel"
readonly plugin_dylib="${plugin_root}/Assets/Plugins/macOS/libnetwork_kernel.dylib"
readonly plugin_bundle="${plugin_root}/Runtime/Resources/gameplay_catalog_bundle/bundle.bytes"

digest() {
  if [[ -f "$1" ]]; then
    /sbin/md5 -q "$1" 2>/dev/null || md5 -q "$1"
  else
    printf 'missing'
  fi
}

stage_file() {
  local source_path="$1"
  local destination_path="$2"
  local label="$3"
  local source_digest destination_digest
  source_digest="$(digest "${source_path}")"
  destination_digest="$(digest "${destination_path}")"
  if [[ "${source_digest}" == "${destination_digest}" ]]; then
    printf '%-7s up to date (md5 %s)\n' "${label}" "${source_digest:0:8}"
    return
  fi
  mkdir -p "$(dirname "${destination_path}")"
  cp "${source_path}" "${destination_path}"
  printf '%-7s staged %s -> %s (md5 %s, was %s)\n' \
    "${label}" "${source_path##*/}" "${destination_path#"${workspace}"/}" \
    "${source_digest:0:8}" "${destination_digest:0:8}"
}

if [[ "${skip_plugin}" == "0" ]]; then
  printf '\n--- plugin staging ---\n'
  # Staging the dylib without the bundle has repeatedly produced a false parity
  # failure (gameplay config lives in the bundle), so both move together.
  stage_file "${dylib}" "${plugin_dylib}" "dylib"
  stage_file "${bundle}" "${plugin_bundle}" "bundle"
fi

# --- 3. Capture runs. ------------------------------------------------------
run_capture() {
  local label="$1"
  local run_dylib="$2"
  local run_bundle="$3"
  printf '\n--- capture: %s ---\n' "${label}"
  "${driver}" \
    --dylib="${run_dylib}" \
    --bundle="${run_bundle}" \
    --manifest="${manifest}" \
    --out-prefix="${output_dir}/${label}_raw" \
    --entry="${entry}" \
    --samples="${sampling}" \
    --tick-rate="${tick_rate}" \
    --path="${path}" \
    --spawn="${spawn}" \
    --entity-template="${entity_template}" \
    --port="${port}" \
    --label="${label}"
}

run_capture "native" "${dylib}" "${bundle}"
if [[ "${skip_plugin}" == "0" ]]; then
  run_capture "plugin" "${plugin_dylib}" "${plugin_bundle}"
fi

# --- 4. Render. ------------------------------------------------------------
printf '\n--- render ---\n'
"${render}" \
  --csv="${output_dir}/native_raw_bones.csv" \
  --out="${output_dir}/native_locomotion.mp4" \
  --title="NATIVE  (built kernel via C ABI, path ${path})" \
  --color="#2b8cbe" \
  --fps="${tick_rate}"
if [[ "${skip_plugin}" == "0" ]]; then
  "${render}" \
    --csv="${output_dir}/plugin_raw_bones.csv" \
    --out="${output_dir}/plugin_locomotion.mp4" \
    --title="PLUGIN  (shipped dylib via C ABI, path ${path})" \
    --color="#31a354" \
    --fps="${tick_rate}"
fi

# --- 5. Report. ------------------------------------------------------------
printf '\n--- report ---\n'
analyze_args=(
  --native-prefix="${output_dir}/native_raw"
  --out="${output_dir}/report.txt"
  --samples="${sampling}"
  --tick-rate="${tick_rate}"
  --path="${path}"
)
if [[ "${skip_plugin}" == "0" ]]; then
  analyze_args+=(--plugin-prefix="${output_dir}/plugin_raw")
fi
if ! "${analyze}" "${analyze_args[@]}"; then
  printf '\nerror: capture checks failed, see %s/report.txt\n' "${output_dir}" >&2
  exit 1
fi

printf '\ncapture complete: %s\n' "${output_dir}"
