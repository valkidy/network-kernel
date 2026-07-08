#!/usr/bin/env bash
set -euo pipefail

CONFIG="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cc_toolchain_config.bzl"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

assert_contains() {
  local needle="$1"
  if ! grep -Fq "$needle" "$CONFIG"; then
    fail "expected $CONFIG to contain: $needle"
  fi
}

if grep -Fq "_MINGW_GCC_VERSION" "$CONFIG"; then
  fail "MinGW builtin include directories must not be pinned to one GCC version"
fi

assert_contains '_MINGW_ROOT + "/lib/gcc/x86_64-w64-mingw32",'
assert_contains '_MINGW_ROOT + "/x86_64-w64-mingw32/include/c++",'
assert_contains '_MINGW_ROOT + "/x86_64-w64-mingw32/include",'
