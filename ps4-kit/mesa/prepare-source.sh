#!/usr/bin/env bash
# Apply the pinned public Mesa PS4 integration patch (or an explicit override),
# then stage the MIT PS4 GNM winsys into the external Mesa source tree.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${PS4_MESA_SOURCE:?source ps4-kit/toolchain/env.sh or set PS4_MESA_SOURCE}"

common="$PS4_MESA_SOURCE/src/amd/common"
meson_file="$common/meson.build"
if [ ! -d "$common" ] || [ ! -f "$meson_file" ]; then
    echo "prepare-mesa-source: PS4_MESA_SOURCE is not a Mesa source tree: $PS4_MESA_SOURCE" >&2
    exit 2
fi

patch_file="${PS4_MESA_PATCH:-$HERE/patches/mesa-ps4.patch}"
if [ ! -f "$patch_file" ]; then
    echo "prepare-mesa-source: PS4 Mesa patch not found: $patch_file" >&2
    exit 3
fi
if ! git -C "$PS4_MESA_SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "prepare-mesa-source: use a Git checkout so the Mesa patch can be checked against its source revision." >&2
    exit 3
fi

# Distinguish a fully applied patch from an unapplied one. If only some hunks
# match, neither check succeeds and we stop rather than leaving a mixed Mesa tree.
if git -C "$PS4_MESA_SOURCE" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    echo "Mesa PS4 patch already applied."
elif git -C "$PS4_MESA_SOURCE" apply --check "$patch_file" >/dev/null 2>&1; then
    git -C "$PS4_MESA_SOURCE" apply "$patch_file"
    echo "Applied Mesa PS4 patch."
else
    echo "prepare-mesa-source: the PS4 Mesa patch is neither cleanly applicable nor fully applied." >&2
    echo "Expected the matching Mesa 26.0.8-family source; set PS4_MESA_PATCH only for a verified alternative." >&2
    exit 4
fi

if ! grep -Fq "'ac_ps4_drm.c'" "$meson_file"; then
    echo "prepare-mesa-source: patch applied but Meson still does not list ac_ps4_drm.c." >&2
    exit 5
fi

install -m 0644 "$HERE/ac_ps4_drm.c" "$common/ac_ps4_drm.c"
echo "Staged PS4 GNM winsys at $common/ac_ps4_drm.c"
sha256sum "$common/ac_ps4_drm.c"
