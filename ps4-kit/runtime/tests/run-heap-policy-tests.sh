#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/ps4-kit/include" \
    "$HERE/ps4_kit_heap_policy.c" "$HERE/tests/test_heap_policy.c" \
    -o "$TMP/test_heap_policy"
"$TMP/test_heap_policy"
