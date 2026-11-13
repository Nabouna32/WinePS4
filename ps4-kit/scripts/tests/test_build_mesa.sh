#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
script="$ROOT/ps4-kit/scripts/build-mesa.sh"
bash -n "$script"
help="$(bash "$script" --help)"
grep -q "Usage: build-mesa.sh" <<< "$help"
grep -q "PS4_MESA_SOURCE" <<< "$help"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/openorbis/bin" "$tmp/openorbis/lib"
mkdir -p "$tmp/sysroot/libc-overlay" "$tmp/sysroot/include/c++/v1" "$tmp/sysroot/lib"
mkdir -p "$tmp/libdrm/include/drm" "$tmp/libdrm/amdgpu"
mkdir -p "$tmp/host-tools/bin" "$tmp/venv/bin"
mkdir -p "$tmp/mesa/src/amd/common" "$tmp/build-root"
mkdir -p "$tmp/build-root/host-tools"

for tool in clang-21 clang++-21 llvm-ar-21 llvm-strip-21 ninja pkg-config; do
    printf '#!/usr/bin/env bash\nexit 0\n' > "$tmp/bin/$tool"
    chmod +x "$tmp/bin/$tool"
done
printf '#!/usr/bin/env bash\nexit 0\n' > "$tmp/openorbis/bin/ld.lld"
chmod +x "$tmp/openorbis/bin/ld.lld"
touch "$tmp/openorbis/link.x" "$tmp/openorbis/lib/crt1.o" "$tmp/openorbis/lib/libunwind.a"
touch "$tmp/sysroot/lib/libc++.a" "$tmp/sysroot/lib/libc++abi.a"
touch "$tmp/host-tools/bin/glslangValidator"

cat > "$tmp/venv/bin/meson" <<'MESON'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" == "setup" ]]; then
    mkdir -p "$2"
    touch "$2/build.ninja"
fi
exit 0
MESON
chmod +x "$tmp/venv/bin/meson"

cat > "$tmp/bin/ninja" <<'NINJA'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$FAKE_NINJA_LOG"
NINJA
chmod +x "$tmp/bin/ninja"

printf "amd_common_files = files(\n  'ac_linux_drm.c',\n)\n" > "$tmp/mesa/src/amd/common/meson.build"
git -C "$tmp/mesa" init -q
git -C "$tmp/mesa" config user.email test@example.invalid
git -C "$tmp/mesa" config user.name "Mesa Build Test"
git -C "$tmp/mesa" add .
git -C "$tmp/mesa" commit -qm "fake Mesa baseline"
cat > "$tmp/mesa.patch" <<'PATCH'
diff --git a/src/amd/common/meson.build b/src/amd/common/meson.build
--- a/src/amd/common/meson.build
+++ b/src/amd/common/meson.build
@@ -1,3 +1,4 @@
 amd_common_files = files(
+  'ac_ps4_drm.c',
   'ac_linux_drm.c',
 )
PATCH
git -C "$tmp/mesa" apply "$tmp/mesa.patch"

export PATH="$tmp/bin:$PATH"
export PS4_KIT="$ROOT/ps4-kit"
export PS4_BUILD_ROOT="$tmp/build-root"
export PS4_MESA_SOURCE="$tmp/mesa"
export PS4_MESA_PATCH="$tmp/mesa.patch"
export PS4_MESA_BUILD="$tmp/build-root/build/mesa"
export PS4_LIBDRM_SOURCE="$tmp/libdrm"
export PS4_MESA_VENV="$tmp/venv"
export PS4_HOST_TOOLS="$tmp/host-tools"
export PS4_SYSROOT="$tmp/sysroot"
export OPENORBIS="$tmp/openorbis"
export PS4_CC=clang-21
export PS4_CXX=clang++-21
export PS4_AR=llvm-ar-21
export PS4_STRIP=llvm-strip-21
export PS4_TARGET=x86_64-pc-freebsd12-elf
export CLANG_VER=21
export JOBS=2
export FAKE_NINJA_LOG="$tmp/ninja.log"

bash "$script" setup >/dev/null

grep -q "c = 'clang-21'" "$tmp/build-root/mesa-ps4-cross.ini"
grep -q -- "-march=btver2" "$tmp/build-root/mesa-ps4-cross.ini"
grep -q "glslangValidator" "$tmp/build-root/mesa-ps4-cross.ini"
cmp "$ROOT/ps4-kit/mesa/ac_ps4_drm.c" "$tmp/mesa/src/amd/common/ac_ps4_drm.c"
test "$(grep -c '^src/' "$tmp/ninja.log")" -eq 19
grep -q "src/amd/vulkan/libvulkan_radeon.a" "$tmp/ninja.log"
grep -q "src/vulkan/wsi/libvulkan_wsi.a" "$tmp/ninja.log"
echo "Mesa build-driver tests passed"
