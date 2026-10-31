#!/usr/bin/env bash
# Build WinePS4's eboot (Wine's Unix side on ps4-kit). Run in WSL.
#   WINEPS4_VERSION=00.01 app/build.sh [clean] [target]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/ps4-kit/toolchain/env.sh"
VERSION="${WINEPS4_VERSION:-00.00}"
B="$PSB/build/wineps4"
[ "${1:-}" = clean ] && { rm -rf "$B"; shift; }
TARGET="${1:-wineps4}"
export PATH="$PSB/host-tools/bin:$PATH" CLANG_VER
need cmake ninja
[ -f "$B/build.ninja" ] || ps4_cmake -S "$HERE" -B "$B" -G Ninja -DWINEPS4_VERSION="$VERSION" > "$PSB/logs/wineps4-configure.log" 2>&1 \
    || { tail -30 "$PSB/logs/wineps4-configure.log"; exit 1; }
# the version of this build, also in a tree configured by an earlier one
grep -q "^WINEPS4_VERSION:STRING=$VERSION\$" "$B/CMakeCache.txt" \
    || cmake -S "$HERE" -B "$B" -DWINEPS4_VERSION="$VERSION" > "$PSB/logs/wineps4-configure.log" 2>&1 \
    || { tail -30 "$PSB/logs/wineps4-configure.log"; exit 1; }
ninja -C "$B" -k 0 -j"$JOBS" "$TARGET" > "$PSB/logs/wineps4-build.log" 2>&1 || { echo "build failed: $PSB/logs/wineps4-build.log"; exit 1; }
echo "built $TARGET"
