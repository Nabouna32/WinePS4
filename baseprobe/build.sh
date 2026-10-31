#!/usr/bin/env bash
# Build the base probe (main.cpp) and package it as out/WinePS4-BaseProbe-vXX.YY.pkg (WINE00003).
# Run in WSL.
#   PROBE_VERSION=00.01 baseprobe/build.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/ps4-kit/toolchain/env.sh"
VERSION="${PROBE_VERSION:-00.00}"
export PATH="$PSB/host-tools/bin:$PATH"
need cmake ninja

STAGE="$PSB/build/wine-baseprobe-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$HERE/sce_sys/icon0.png" "$STAGE/sce_sys/"

# role, base, file in the package
build() {
    local role="$1" base="$2" file="$3" b="$PSB/build/wine-baseprobe-$1"
    rm -rf "$b"
    ps4_cmake -S "$HERE" -B "$b" -G Ninja -DPROBE_ROLE="$role" -DPROBE_BASE="$base" > "$PSB/logs/wine-baseprobe-$role.log" 2>&1 \
        || { tail -30 "$PSB/logs/wine-baseprobe-$role.log"; exit 1; }
    ninja -C "$b" -j"$JOBS" >> "$PSB/logs/wine-baseprobe-$role.log" 2>&1 \
        || { grep -E "error|undefined" "$PSB/logs/wine-baseprobe-$role.log" | head -40; exit 1; }
    cp "$b/baseprobe_eboot/eboot.bin" "$STAGE/$file"
    [ -z "$base" ] || python3 "$HERE/set-exec-type.py" "$STAGE/$file"
    llvm-readelf -l "$b/baseprobe" | grep -m2 LOAD
}
build launcher "" eboot.bin
build high-1g 0x60000000 high-1g.bin
build high-512m 0x20000000 high-512m.bin

PKG="$(NO_RIGHT_SPRX=1 bash "$PS4_KIT/scripts/make-pkg.sh" "$STAGE" WINE00003 "Base probe" "$VERSION" WINEBASEPROBE "$PSB/out" | tail -1)"
OUT="$HERE/../out"
mkdir -p "$OUT"
NAME="WinePS4-BaseProbe-v$VERSION.pkg"
cp "$PKG" "$OUT/$NAME"
ls -la "$OUT/$NAME"
