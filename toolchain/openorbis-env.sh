#!/usr/bin/env bash
# Clean-room OpenOrbis bootstrap for WinePS4 research.
# This replaces only the public build-environment portion of ps4-kit; it does NOT
# implement ps4_kit_app(), RADV/WSI, runtime hooks or PKG packaging.
#
# Usage:
#   export OPENORBIS=/opt/pacbrew/ps4/openorbis
#   export CLANG_VER=12   # or the installed LLVM version used by your OpenOrbis setup
#   source toolchain/openorbis-env.sh
#
# Then use ps4_cmake as the public PacBrew/OpenOrbis convention does.

set -euo pipefail

export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export PSB="${PSB:-$HOME/wineps4-build}"
export PS4_SYSROOT="${PS4_SYSROOT:-$PSB/sysroot}"
export JOBS="${JOBS:-$(nproc)}"

if [[ -z "${CLANG_VER:-}" ]]; then
    echo "openorbis-env: set CLANG_VER to the LLVM tool version installed on this host" >&2
    return 1 2>/dev/null || exit 1
fi
export CLANG_VER

for required in \
    "$OPENORBIS/ps4vars.sh" \
    "$OPENORBIS/cmake/ps4.cmake" \
    "$OPENORBIS/link.x" \
    "$OPENORBIS/bin/create-fself"; do
    if [[ ! -e "$required" ]]; then
        echo "openorbis-env: missing OpenOrbis component: $required" >&2
        return 1 2>/dev/null || exit 1
    fi
done

# PacBrew's environment supplies compiler, linker, SDK and platform flags.
# shellcheck disable=SC1090
source "$OPENORBIS/ps4vars.sh"

# Keep the compiler version explicit: WinePS4 invokes versioned LLVM utilities
# (ld.lld-$CLANG_VER, llvm-objcopy-$CLANG_VER) in its CMake build.
for tool in "clang-$CLANG_VER" "clang++-$CLANG_VER" "ld.lld-$CLANG_VER" \
            "llvm-objcopy-$CLANG_VER" "llvm-ar-$CLANG_VER"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "openorbis-env: required host tool not found in PATH: $tool" >&2
        return 1 2>/dev/null || exit 1
    fi
done

need() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "openorbis-env: missing host tool: $tool" >&2
            return 1
        }
    done
}

ps4_cmake() {
    need cmake
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake \
        -DCMAKE_TOOLCHAIN_FILE="$OPENORBIS/cmake/ps4.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "$@"
}

export -f need ps4_cmake
echo "OpenOrbis environment ready: $OPENORBIS (LLVM $CLANG_VER)"
echo "Build tree: $PSB"
echo "Sysroot overlay: $PS4_SYSROOT"
echo "Note: ps4-kit CMake/runtime/Mesa/packaging compatibility is not implemented yet."
