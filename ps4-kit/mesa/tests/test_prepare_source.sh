#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/mesa/src/amd/common"
printf "amd_common_files = files(\n  'ac_ps4_drm.c',\n)\n" > "$tmp/mesa/src/amd/common/meson.build"
PS4_MESA_SOURCE="$tmp/mesa" bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >/dev/null
cmp "$ROOT/ps4-kit/mesa/ac_ps4_drm.c" "$tmp/mesa/src/amd/common/ac_ps4_drm.c"

mkdir -p "$tmp/unpatched/src/amd/common"
printf "amd_common_files = files(\n  'ac_linux_drm.c',\n)\n" > "$tmp/unpatched/src/amd/common/meson.build"
if PS4_MESA_SOURCE="$tmp/unpatched" bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >"$tmp/out" 2>&1; then
    echo "expected missing PS4 Meson integration to fail" >&2
    exit 1
fi
grep -q "Apply the matching PS4 Mesa Meson integration patch first" "$tmp/out"
echo "Mesa GNM source staging tests passed"
