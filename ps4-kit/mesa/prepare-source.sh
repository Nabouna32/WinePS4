#!/usr/bin/env bash
# Stage the MIT PS4 GNM winsys into an already PS4-patched Mesa source tree.
# The Meson integration patch is revision-sensitive and is deliberately not guessed here.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${PS4_MESA_SOURCE:?source ps4-kit/toolchain/env.sh or set PS4_MESA_SOURCE}"

common="$PS4_MESA_SOURCE/src/amd/common"
meson_file="$common/meson.build"
if [ ! -d "$common" ] || [ ! -f "$meson_file" ]; then
    echo "prepare-mesa-source: PS4_MESA_SOURCE is not a Mesa source tree: $PS4_MESA_SOURCE" >&2
    exit 2
fi

if ! grep -Fq "'ac_ps4_drm.c'" "$meson_file"; then
    echo "prepare-mesa-source: Mesa's src/amd/common/meson.build does not include 'ac_ps4_drm.c'." >&2
    echo "Apply the matching PS4 Mesa Meson integration patch first; refusing to guess or edit Meson files." >&2
    exit 3
fi

install -m 0644 "$HERE/ac_ps4_drm.c" "$common/ac_ps4_drm.c"
echo "Staged PS4 GNM winsys at $common/ac_ps4_drm.c"
sha256sum "$common/ac_ps4_drm.c"
