#!/usr/bin/env bash
# Build the process probe (main.cpp) as out/WinePS4-ProcessProbe-vXX.YY.pkg (WINE00004). Run in WSL.
#   PROBE_VERSION=00.01 processprobe/build.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/ps4-kit/toolchain/env.sh"
VERSION="${PROBE_VERSION:-00.00}"
export PATH="$PSB/host-tools/bin:$PATH"
need cmake ninja

STAGE="$PSB/build/wine-processprobe-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$HERE/../probe/sce_sys/icon0.png" "$STAGE/sce_sys/"

build() {  # build <name> <child: ON/OFF> <file in the package>
    local b="$PSB/build/wine-processprobe-$1"
    rm -rf "$b"
    ps4_cmake -S "$HERE" -B "$b" -G Ninja -DPROBE_CHILD="$2" > "$PSB/logs/wine-processprobe-$1.log" 2>&1 \
        || { tail -30 "$PSB/logs/wine-processprobe-$1.log"; exit 1; }
    ninja -C "$b" -j"$JOBS" >> "$PSB/logs/wine-processprobe-$1.log" 2>&1 \
        || { grep -E "error|undefined" "$PSB/logs/wine-processprobe-$1.log" | head -40; exit 1; }
    cp "$b/processprobe_eboot/eboot.bin" "$STAGE/$3"
}
build parent OFF eboot.bin
build child ON child.bin

PKG="$(NO_RIGHT_SPRX=1 bash "$PS4_KIT/scripts/make-pkg.sh" "$STAGE" WINE00004 "Process probe" "$VERSION" WINEPROCPROBE "$PSB/out" | tail -1)"
OUT="$HERE/../out"
mkdir -p "$OUT"
NAME="WinePS4-ProcessProbe-v$VERSION.pkg"
cp "$PKG" "$OUT/$NAME"
ls -la "$OUT/$NAME"
