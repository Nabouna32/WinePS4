#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Already-patched Mesa tree: stage the winsys without changing Meson files.
mkdir -p "$tmp/mesa/src/amd/common"
printf "amd_common_files = files(\n  'ac_ps4_drm.c',\n)\n" > "$tmp/mesa/src/amd/common/meson.build"
PS4_MESA_SOURCE="$tmp/mesa" bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >/dev/null
cmp "$ROOT/ps4-kit/mesa/ac_ps4_drm.c" "$tmp/mesa/src/amd/common/ac_ps4_drm.c"

# Unpatched Git checkout: a supplied, matching patch is checked, applied, then the winsys is staged.
mkdir -p "$tmp/unpatched/src/amd/common"
git -C "$tmp/unpatched" init -q
printf "amd_common_files = files(\n  'ac_linux_drm.c',\n)\n" > "$tmp/unpatched/src/amd/common/meson.build"
cat > "$tmp/test.patch" <<'PATCH'
diff --git a/src/amd/common/meson.build b/src/amd/common/meson.build
--- a/src/amd/common/meson.build
+++ b/src/amd/common/meson.build
@@ -1,3 +1,4 @@
 amd_common_files = files(
+  'ac_ps4_drm.c',
   'ac_linux_drm.c',
 )
PATCH
PS4_MESA_SOURCE="$tmp/unpatched" PS4_MESA_PATCH="$tmp/test.patch" \
    bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >/dev/null
grep -Fq "'ac_ps4_drm.c'" "$tmp/unpatched/src/amd/common/meson.build"
cmp "$ROOT/ps4-kit/mesa/ac_ps4_drm.c" "$tmp/unpatched/src/amd/common/ac_ps4_drm.c"

# A patch against the wrong source must fail before changing the tree.
mkdir -p "$tmp/wrong/src/amd/common"
git -C "$tmp/wrong" init -q
printf "not the expected Mesa file\n" > "$tmp/wrong/src/amd/common/meson.build"
if PS4_MESA_SOURCE="$tmp/wrong" PS4_MESA_PATCH="$tmp/test.patch" \
    bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >"$tmp/out" 2>&1; then
    echo "expected a mismatched Mesa patch to fail" >&2
    exit 1
fi
grep -q "patch does not apply to this source tree" "$tmp/out"
test ! -e "$tmp/wrong/src/amd/common/ac_ps4_drm.c"

echo "Mesa GNM source staging tests passed"
