#!/usr/bin/env bash
set -u

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly UNITY_EXECUTABLE="/Applications/Unity/Hub/Editor/6000.4.3f1/Unity.app/Contents/MacOS/Unity"
readonly OPENSSL_LIB_DIR="/opt/homebrew/opt/openssl@3/lib"

failure_count=0

pass() {
  printf 'PASS  %s\n' "$1"
}

warn() {
  printf 'WARN  %s\n' "$1"
}

fail() {
  printf 'FAIL  %s\n' "$1"
  failure_count=$((failure_count + 1))
}

require_command() {
  local command_name="$1"
  if command -v "${command_name}" >/dev/null 2>&1; then
    pass "${command_name}: $(command -v "${command_name}")"
  else
    fail "${command_name} is not available in PATH"
  fi
}

printf 'Multi-Bone Locomotion Phase 0 preflight\n'
printf 'Repository: %s\n' "${REPO_ROOT}"

if [[ "$(uname -s)" == "Darwin" ]]; then
  pass "host OS is macOS"
else
  fail "host OS must be macOS for the Phase 0 required gate"
fi

if [[ "$(uname -m)" == "arm64" ]]; then
  pass "host architecture is arm64"
else
  fail "host architecture must be arm64 for the Phase 0 required gate"
fi

for required_command in bazel git clang++ node npm curl shasum; do
  require_command "${required_command}"
done

if command -v bazel >/dev/null 2>&1; then
  expected_bazel_version="$(tr -d '[:space:]' < "${REPO_ROOT}/.bazelversion")"
  actual_bazel_version="$(bazel --version 2>/dev/null | awk '{print $2}')"
  if [[ "${actual_bazel_version}" == "${expected_bazel_version}" ]]; then
    pass "Bazel version is ${actual_bazel_version}"
  else
    fail "Bazel version is ${actual_bazel_version:-unknown}; expected ${expected_bazel_version}"
  fi
fi

if command -v clang++ >/dev/null 2>&1; then
  if printf '#if __cplusplus < 202002L\n#error C++20 unavailable\n#endif\n' |
      clang++ -std=c++20 -x c++ -fsyntax-only - >/dev/null 2>&1; then
    pass "clang++ accepts C++20"
  else
    fail "clang++ does not accept -std=c++20"
  fi
fi

if [[ -f "${OPENSSL_LIB_DIR}/libssl.3.dylib" &&
      -f "${OPENSSL_LIB_DIR}/libcrypto.3.dylib" ]]; then
  pass "Homebrew OpenSSL 3 libraries are available"
else
  fail "missing Homebrew OpenSSL 3 libraries under ${OPENSSL_LIB_DIR}"
fi

if grep -q '^common --noenable_bzlmod$' "${REPO_ROOT}/.bazelrc"; then
  pass "WORKSPACE dependency mode is enabled"
else
  fail "${REPO_ROOT}/.bazelrc must preserve common --noenable_bzlmod"
fi

if [[ -x "${UNITY_EXECUTABLE}" ]]; then
  pass "optional Unity Editor found at ${UNITY_EXECUTABLE}"
else
  warn "Unity 6000.4.3f1 not found; Unity validation remains deferred to Phase 5"
fi

if (( failure_count > 0 )); then
  printf 'Preflight failed with %d required check(s).\n' "${failure_count}"
  exit 1
fi

printf 'Preflight passed.\n'
