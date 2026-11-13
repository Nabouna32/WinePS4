#!/usr/bin/env bash
# Build the public Mesa 26.0.8-family PS4 RADV archive set for the reconstructed kit.
# Based on the public DolphinPS4 Mesa build recipe; paths/tools come from ps4-kit env.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
KIT_ROOT="$(cd "$HERE/.." && pwd)"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<'HELP'
Usage: build-mesa.sh [setup] [ninja-arguments...]

Requires the OpenOrbis environment (source ps4-kit/toolchain/env.sh first, or set CLANG_VER).
Applies the pinned Mesa PS4 patch if it matches PS4_MESA_SOURCE, stages ac_ps4_drm.c,
configures a static RADV cross-build, and builds the archive targets used by WinePS4.

Environment overrides:
  PS4_MESA_SOURCE, PS4_MESA_BUILD, PS4_MESA_PATCH, PS4_LIBDRM_SOURCE
  PS4_MESA_VENV, PS4_HOST_TOOLS, PS4_MESA_OVERLAY, JOBS
HELP
    exit 0
fi

if [[ -z "${PS4_KIT:-}" ]]; then
    # shellcheck disable=SC1091
    source "$KIT_ROOT/toolchain/env.sh"
fi

ROOT="$PS4_BUILD_ROOT"
SYSROOT="$PS4_SYSROOT"
MESA="$PS4_MESA_SOURCE"
BUILD="$PS4_MESA_BUILD"
LIBDRM="${PS4_LIBDRM_SOURCE:-$ROOT/src/libdrm}"
VENV="${PS4_MESA_VENV:-$ROOT/mesa-venv}"
HOST_TOOLS="${PS4_HOST_TOOLS:-$ROOT/host-tools}"
OVERLAY="${PS4_MESA_OVERLAY:-$KIT_ROOT/mesa/mesa-overlay}"
PATCH_FILE="${PS4_MESA_PATCH:-$KIT_ROOT/mesa/patches/mesa-ps4.patch}"
STRIP_TOOL="${PS4_STRIP:-llvm-strip-$CLANG_VER}"
GLSLANG="${GLSLANG_VALIDATOR:-$HOST_TOOLS/bin/glslangValidator}"
MESON="$VENV/bin/meson"

setup=0
if [[ "${1:-}" == "setup" ]]; then
    setup=1
    shift
fi

fail() {
    echo "ps4-kit build-mesa: $*" >&2
    exit 1
}
require_file() { [[ -f "$1" ]] || fail "missing file: $1"; }
require_dir() { [[ -d "$1" ]] || fail "missing directory: $1"; }
require_tool() { command -v "$1" >/dev/null 2>&1 || fail "missing host/target tool: $1"; }

require_dir "$MESA"
require_dir "$LIBDRM"
require_dir "$SYSROOT/libc-overlay"
require_dir "$SYSROOT/include/c++/v1"
require_file "$SYSROOT/lib/libc++.a"
require_file "$SYSROOT/lib/libc++abi.a"
require_file "$OPENORBIS/link.x"
require_file "$OPENORBIS/lib/crt1.o"
require_file "$OPENORBIS/lib/libunwind.a"
require_file "$OPENORBIS/bin/ld.lld"
require_dir "$OVERLAY"
require_file "$OVERLAY/sys/ioccom.h"
require_file "$GLSLANG"
require_file "$MESON"
require_tool ninja
require_tool pkg-config
require_tool "$PS4_CC"
require_tool "$PS4_CXX"
require_tool "$PS4_AR"
require_tool "$STRIP_TOOL"

export PATH="$VENV/bin:$HOST_TOOLS/bin:$PATH"

# Apply the source patch only if it matches this Mesa checkout; stage the MIT GNM winsys.
PS4_MESA_SOURCE="$MESA" PS4_MESA_PATCH="$PATCH_FILE" \
    bash "$KIT_ROOT/mesa/prepare-source.sh"

# Header-only libdrm metadata: the PS4 uses GNM, not a Linux DRM device.
PC_DIR="$ROOT/mesa-pkgconfig"
mkdir -p "$PC_DIR"
cat > "$PC_DIR/libdrm_amdgpu.pc" <<EOF
Name: libdrm_amdgpu
Description: libdrm UAPI headers only (PS4 uses GNM, not Linux DRM)
Version: 2.4.125
Cflags: -I$LIBDRM -I$LIBDRM/include/drm -I$LIBDRM/amdgpu
Libs:
EOF

CROSS="$ROOT/mesa-ps4-cross.ini"
cat > "$CROSS" <<EOF
[binaries]
c = '$PS4_CC'
cpp = '$PS4_CXX'
ar = '$PS4_AR'
strip = '$STRIP_TOOL'
pkg-config = 'pkg-config'
glslangValidator = '$GLSLANG'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PC_DIR'

[built-in options]
c_args = [
  '-target', '$PS4_TARGET', '-D_GNU_SOURCE', '-D__PS4__', '-D__OPENORBIS__',
  '-D__ORBIS__', '-DPS4', '-D__BSD_VISIBLE', '-D_BSD_SOURCE', '-fPIC',
  '-funwind-tables', '-march=btver2', '-isysroot', '$OPENORBIS',
  '-isystem', '$SYSROOT/libc-overlay', '-isystem', '$OVERLAY',
  '-I$LIBDRM', '-I$LIBDRM/include/drm', '-I$LIBDRM/amdgpu',
  '-isystem', '$OPENORBIS/include', '-I$OPENORBIS/usr/include'
]
cpp_args = [
  '-nostdinc++', '-isystem', '$SYSROOT/include/c++/v1',
  '-target', '$PS4_TARGET', '-D_GNU_SOURCE', '-D__PS4__', '-D__OPENORBIS__',
  '-D__ORBIS__', '-DPS4', '-D__BSD_VISIBLE', '-D_BSD_SOURCE', '-fPIC',
  '-funwind-tables', '-march=btver2', '-isysroot', '$OPENORBIS',
  '-isystem', '$SYSROOT/libc-overlay', '-isystem', '$OVERLAY',
  '-I$LIBDRM', '-I$LIBDRM/include/drm', '-I$LIBDRM/amdgpu',
  '-isystem', '$OPENORBIS/include', '-I$OPENORBIS/usr/include'
]
c_link_args = [
  '-fuse-ld=$OPENORBIS/bin/ld.lld', '-nostdlib', '-Wl,-m,elf_x86_64',
  '-Wl,-pie', '-Wl,--eh-frame-hdr', '-Wl,--script,$OPENORBIS/link.x',
  '-L$OPENORBIS/lib', '$OPENORBIS/lib/crt1.o', '-lkernel', '-lc',
  '-lclang_rt.builtins-x86_64', '-lSceLibcInternal'
]
cpp_link_args = [
  '-fuse-ld=$OPENORBIS/bin/ld.lld', '-nostdlib', '-Wl,-m,elf_x86_64',
  '-Wl,-pie', '-Wl,--eh-frame-hdr', '-Wl,--script,$OPENORBIS/link.x',
  '-L$OPENORBIS/lib', '$OPENORBIS/lib/crt1.o', '-lkernel', '-lc',
  '-lclang_rt.builtins-x86_64', '-lSceLibcInternal',
  '$SYSROOT/lib/libc++.a', '$SYSROOT/lib/libc++abi.a', '$OPENORBIS/lib/libunwind.a'
]

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

if [[ "$setup" == "1" || ! -f "$BUILD/build.ninja" ]]; then
    rm -rf "$BUILD"
    PKG_CONFIG_LIBDIR="$PC_DIR" "$MESON" setup "$BUILD" "$MESA" --cross-file "$CROSS" \
        -Dbuildtype=release -Db_ndebug=true -Ddefault_library=static \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dllvm=disabled \
        -Damd-use-llvm=false -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dzstd=disabled \
        -Dexpat=disabled -Dxmlconfig=disabled -Dvalgrind=disabled \
        -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
        -Dshader-cache=disabled -Dvulkan-layers= -Dtools= -Dvideo-codecs= \
        -Dspirv-tools=disabled -Dperfetto=false -Dselinux=false
fi

build_id="$(git -C "$MESA" rev-parse HEAD)"
PKG_CONFIG_LIBDIR="$PC_DIR" "$MESON" configure "$BUILD" -Dradv-build-id="$build_id" >/dev/null

# Build the public recipe's static RADV target set. The final WinePS4 link closure
# is the 16-archive subset in mesa/archives.cmake.
libs=(
    src/amd/vulkan/libvulkan_radeon.a
    src/amd/addrlib/libaddrlib.a
    src/amd/common/libamd_common.a
    src/amd/compiler/libaco.a
    src/c11/impl/libmesa_util_c11.a
    src/compiler/libcompiler.a
    src/compiler/nir/libnir.a
    src/compiler/spirv/libvtn.a
    src/util/blake3/libblake3.a
    src/util/libmesa_util.a
    src/util/libmesa_util_clflush.a
    src/util/libmesa_util_clflushopt.a
    src/util/libmesa_util_simd.a
    src/util/libparson.a
    src/util/libxmlconfig.a
    src/vulkan/runtime/libvulkan_instance.a
    src/vulkan/runtime/libvulkan_runtime.a
    src/vulkan/util/libvulkan_util.a
    src/vulkan/wsi/libvulkan_wsi.a
)
ninja -C "$BUILD" -j "$JOBS" "$@" "${libs[@]}"
echo "Mesa RADV archives built in $BUILD"
