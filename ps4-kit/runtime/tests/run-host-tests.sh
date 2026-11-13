#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror -pthread \
    -I"$ROOT/ps4-kit/include" \
    "$HERE/ps4_kit_runtime.c" "$HERE/tests/test_runtime.c" \
    -Wl,--wrap=pthread_create -o "$TMP/test_runtime"
"$TMP/test_runtime"
