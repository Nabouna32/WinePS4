#!/usr/bin/env bash
# Build the Wine probe and package it as out/WinePS4-Probe-vXX.YY.pkg (title WINE00001). Run in WSL.
#   PROBE_VERSION=00.01 probe/build.sh [clean]
# Then, from Windows (Git Bash): ~/ps4-kit/scripts/upload.sh out/WinePS4-Probe-vXX.YY.pkg <console-ip>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/ps4-kit/toolchain/env.sh"
VERSION="${PROBE_VERSION:-00.00}"
B="$PSB/build/wine-probe"
[ "${1:-}" = clean ] && rm -rf "$B"
export PATH="$PSB/host-tools/bin:$PATH"
need cmake ninja

ps4_cmake -S "$HERE" -B "$B" -G Ninja -DPROBE_VERSION="$VERSION" > "$PSB/logs/wine-probe-configure.log" \
    || { tail -30 "$PSB/logs/wine-probe-configure.log"; exit 1; }
ninja -C "$B" -j"$JOBS" > "$PSB/logs/wine-probe-build.log" 2>&1 \
    || { grep -E "error|undefined" "$PSB/logs/wine-probe-build.log" | head -40; exit 1; }

STAGE="$B/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$B/wine_probe_eboot/eboot.bin" "$STAGE/eboot.bin"
cp "$HERE/sce_sys/icon0.png" "$STAGE/sce_sys/"
PKG="$(NO_RIGHT_SPRX=1 bash "$PS4_KIT/scripts/make-pkg.sh" "$STAGE" WINE00001 "Wine Probe" "$VERSION" WINEPROBE "$PSB/out" | tail -1)"
OUT="$HERE/../out"
mkdir -p "$OUT"
NAME="WinePS4-Probe-v$VERSION.pkg"
cp "$PKG" "$OUT/$NAME"
mkdir -p "$PSB/oelf"
cp "$B/wine_probe_eboot/wine_probe.oelf" "$PSB/oelf/wine-probe-v$VERSION.oelf"
ls -la "$OUT/$NAME"
