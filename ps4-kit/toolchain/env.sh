#!/usr/bin/env bash
# Reconstructed ps4-kit environment for the unchanged WinePS4 build scripts.
#
# Install this directory at $HOME/ps4-kit (a symlink is sufficient), then:
#   source "$HOME/ps4-kit/toolchain/env.sh"
#
# The public OpenOrbis/PacBrew environment is the base. Kit-specific runtime,
# Mesa/VideoOut and packaging pieces are tracked separately until reproduced.

# This path also works when $HOME/ps4-kit is a symlink into the WinePS4 checkout.
export PS4_KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export PSB="${PSB:-$HOME/wineps4-build}"
# Match the public PS4 Mesa build-tree contract used by related OpenOrbis ports.
export PS4_BUILD_ROOT="${PS4_BUILD_ROOT:-$PSB}"
export PS4_SYSROOT="${PS4_SYSROOT:-$PS4_BUILD_ROOT/sysroot}"
export PS4_MESA_SOURCE="${PS4_MESA_SOURCE:-$PS4_BUILD_ROOT/src/mesa}"
export PS4_MESA_BUILD="${PS4_MESA_BUILD:-$PS4_BUILD_ROOT/build/mesa}"
export PS4_TARGET="${PS4_TARGET:-x86_64-pc-freebsd12-elf}"
export JOBS="${JOBS:-$(nproc)}"

# WinePS4 calls version-suffixed LLVM utilities directly (ld.lld-$CLANG_VER,
# llvm-objcopy-$CLANG_VER). Do not silently select a different toolchain.
if [[ -z "${CLANG_VER:-}" ]]; then
    echo "ps4-kit: set CLANG_VER to the LLVM major version used by your OpenOrbis install" >&2
    return 1 2>/dev/null || exit 1
fi
export CLANG_VER
export PS4_CC="${PS4_CC:-clang-$CLANG_VER}"
export PS4_CXX="${PS4_CXX:-clang++-$CLANG_VER}"
export PS4_LD="${PS4_LD:-ld.lld-$CLANG_VER}"
export PS4_AR="${PS4_AR:-llvm-ar-$CLANG_VER}"
export PS4_RANLIB="${PS4_RANLIB:-llvm-ranlib-$CLANG_VER}"
export PS4_OBJCOPY="${PS4_OBJCOPY:-llvm-objcopy-$CLANG_VER}"

for required in \
    "$OPENORBIS/ps4vars.sh" \
    "$OPENORBIS/cmake/ps4.cmake" \
    "$OPENORBIS/link.x" \
    "$OPENORBIS/bin/create-fself"; do
    if [[ ! -e "$required" ]]; then
        echo "ps4-kit: missing OpenOrbis component: $required" >&2
        return 1 2>/dev/null || exit 1
    fi
done

# PacBrew's own setup provides SDK paths and system library flags.
# Preserve the kit build-tree overlay: ps4vars.sh may set PS4_SYSROOT to the
# OpenOrbis SDK root, while WinePS4's target headers/libraries live in PSB/sysroot.
_PS4_KIT_SYSROOT="$PS4_SYSROOT"
_PS4_KIT_MESA_SOURCE="$PS4_MESA_SOURCE"
_PS4_KIT_MESA_BUILD="$PS4_MESA_BUILD"
# shellcheck disable=SC1090
source "$OPENORBIS/ps4vars.sh"
export PS4_SYSROOT="$_PS4_KIT_SYSROOT"
export PS4_MESA_SOURCE="$_PS4_KIT_MESA_SOURCE"
export PS4_MESA_BUILD="$_PS4_KIT_MESA_BUILD"
unset _PS4_KIT_SYSROOT _PS4_KIT_MESA_SOURCE _PS4_KIT_MESA_BUILD

export PS4_KIT OPENORBIS PSB PS4_BUILD_ROOT PS4_SYSROOT PS4_MESA_SOURCE PS4_MESA_BUILD PS4_TARGET CLANG_VER
export PS4_CC PS4_CXX PS4_LD PS4_AR PS4_RANLIB PS4_OBJCOPY JOBS
export PS4_BASE_FLAGS="${PS4_BASE_FLAGS:-"--target=$PS4_TARGET -D_GNU_SOURCE -D__PS4__ -D__ORBIS__ -D__OPENORBIS__ -fPIC -funwind-tables --sysroot=$OPENORBIS -isystem $PS4_SYSROOT/libc-overlay -isystem $OPENORBIS/include"}"
export PS4_CXX_FLAGS="${PS4_CXX_FLAGS:--nostdinc++ -isystem $PS4_SYSROOT/include/c++/v1}"

export PATH="$OPENORBIS/bin:$PSB/host-tools/bin:$PATH"

for tool in "$PS4_CC" "$PS4_CXX" "$PS4_LD" "$PS4_AR" "$PS4_RANLIB" "$PS4_OBJCOPY" cmake ninja python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ps4-kit: required tool not found in PATH: $tool" >&2
        return 1 2>/dev/null || exit 1
    fi
done

need() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "ps4-kit: missing host tool: $tool" >&2
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
echo "ps4-kit environment: $PS4_KIT"
echo "OpenOrbis: $OPENORBIS"
echo "LLVM: $CLANG_VER ($PS4_CC / $PS4_CXX)"
echo "Build root: $PSB"
echo "Target sysroot overlay: $PS4_SYSROOT"
echo "Mesa source/build: $PS4_MESA_SOURCE / $PS4_MESA_BUILD"
