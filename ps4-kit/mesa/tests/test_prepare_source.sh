#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

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

make_mesa_tree() {
    local tree="$1"
    mkdir -p "$tree/src/amd/common"
    printf "amd_common_files = files(\n  'ac_linux_drm.c',\n)\n" > "$tree/src/amd/common/meson.build"
    git -C "$tree" init -q
    git -C "$tree" config user.email test@example.invalid
    git -C "$tree" config user.name "Mesa Patch Test"
    git -C "$tree" add .
    git -C "$tree" commit -qm "fake Mesa baseline"
}

# Unapplied patch: helper checks then applies the patch before staging the winsys.
make_mesa_tree "$tmp/unpatched"
PS4_MESA_SOURCE="$tmp/unpatched" PS4_MESA_PATCH="$tmp/test.patch" \
    bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >/dev/null
grep -Fq "'ac_ps4_drm.c'" "$tmp/unpatched/src/amd/common/meson.build"
cmp "$ROOT/ps4-kit/mesa/ac_ps4_drm.c" "$tmp/unpatched/src/amd/common/ac_ps4_drm.c"

# Already-applied patch: reverse-check detects it and avoids applying twice.
make_mesa_tree "$tmp/already"
git -C "$tmp/already" apply "$tmp/test.patch"
PS4_MESA_SOURCE="$tmp/already" PS4_MESA_PATCH="$tmp/test.patch" \
    bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >/dev/null
cmp "$ROOT/ps4-kit/mesa/ac_ps4_drm.c" "$tmp/already/src/amd/common/ac_ps4_drm.c"

# Mismatched source: neither forward nor reverse patch check succeeds.
make_mesa_tree "$tmp/wrong"
printf "not the expected Mesa file\n" > "$tmp/wrong/src/amd/common/meson.build"
if PS4_MESA_SOURCE="$tmp/wrong" PS4_MESA_PATCH="$tmp/test.patch" \
    bash "$ROOT/ps4-kit/mesa/prepare-source.sh" >"$tmp/out" 2>&1; then
    echo "expected a mismatched Mesa patch to fail" >&2
    exit 1
fi
grep -q "neither cleanly applicable nor fully applied" "$tmp/out"
test ! -e "$tmp/wrong/src/amd/common/ac_ps4_drm.c"

echo "Mesa GNM source staging tests passed"
