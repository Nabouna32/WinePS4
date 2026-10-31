#!/usr/bin/env bash
# Package WinePS4: the eboot (app/build.sh), the host build's Windows DLLs and data, the prefix made
# by the host build (scripts/make-prefix.sh) and the test programs, as out/WinePS4-vXX.YY.pkg
# (title WINE00002). Run in WSL.
#   WINEPS4_VERSION=00.01 app/package.sh
#
# /app0/wine/lib/wine/x86_64-windows   Wine's PE DLLs and programs (no import libraries)
# /app0/wine/share/wine                nls files, wine.inf
# /app0/wine/prefix                    the prefix without dosdevices/ and without system32's copies of
#                                      the DLLs above (system32-from-lib.txt lists them: the first
#                                      run copies them into the console's prefix)
# /app0/wine/hello.exe                 the test program run when /data/WinePS4/run.txt is absent
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
source "$HOME/ps4-kit/toolchain/env.sh"
VERSION="${WINEPS4_VERSION:-00.00}"
B="$PSB/build/wineps4"
HOSTWINE="$HOME/wine-ps4-build/install/wine-host"
PREFIX="$HOME/wine-ps4-build/prefix"
[ -f "$B/wineps4_eboot/eboot.bin" ] || { echo "build first: app/build.sh all" >&2; exit 1; }
[ -f "$PREFIX/system.reg" ] || { echo "no prefix at $PREFIX" >&2; exit 1; }

STAGE="$B/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys" "$STAGE/wine/lib/wine/x86_64-windows" "$STAGE/wine/share"
cp "$B/wineps4_eboot/eboot.bin" "$STAGE/eboot.bin"
cp "$ROOT/app/sce_sys/icon0.png" "$STAGE/sce_sys/"
rsync -a --exclude '*.a' "$HOSTWINE/lib/wine/x86_64-windows/" "$STAGE/wine/lib/wine/x86_64-windows/"
rsync -a "$HOSTWINE/share/wine" "$STAGE/wine/share/"
rsync -a --exclude dosdevices "$PREFIX/" "$STAGE/wine/prefix/"

LIB="$STAGE/wine/lib/wine/x86_64-windows"
SYS32="$STAGE/wine/prefix/drive_c/windows/system32"
: > "$STAGE/wine/system32-from-lib.txt"
for f in "$SYS32"/*; do
    name="$(basename "$f")"
    if [ -f "$f" ] && [ -f "$LIB/$name" ] && cmp -s "$f" "$LIB/$name"; then
        echo "$name" >> "$STAGE/wine/system32-from-lib.txt"
        rm "$f"
    fi
done
cp "$ROOT/tests/hello.exe" "$STAGE/wine/hello.exe"
mkdir -p "$STAGE/fonts"
cp "$ROOT"/app/fonts/* "$STAGE/fonts/"   # the desktop's (Roboto, OFL)
echo "staged $(wc -l < "$STAGE/wine/system32-from-lib.txt") system32 DLLs from lib, $(du -sh "$STAGE" | cut -f1) in all"

PKG="$(NO_RIGHT_SPRX=1 bash "$PS4_KIT/scripts/make-pkg.sh" "$STAGE" WINE00002 "WinePS4" "$VERSION" WINEPS4 "$PSB/out" | tail -1)"
mkdir -p "$ROOT/out"
NAME="WinePS4-v$VERSION.pkg"
cp "$PKG" "$ROOT/out/$NAME"
mkdir -p "$PSB/oelf"
cp "$B/wineps4_eboot/wineps4.oelf" "$PSB/oelf/wineps4-v$VERSION.oelf"
cp "$B/wineps4" "$PSB/oelf/wineps4-v$VERSION.elf"
ls -la "$ROOT/out/$NAME"
