#!/usr/bin/env bash
set -euo pipefail
KIT_ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

OO="$TMP/openorbis"
TOOLS="$TMP/tools"
PSB="$TMP/wineps4-build"
mkdir -p "$OO/cmake" "$OO/bin" "$TOOLS" "$PSB/host-tools/bin"
: > "$OO/cmake/ps4.cmake"
: > "$OO/link.x"
cat > "$OO/ps4vars.sh" <<'STUB'
# Simulate the SDK script setting the wrong sysroot for this kit's overlay.
export PS4_SYSROOT=/openorbis/sdk/sysroot
STUB
cat > "$OO/bin/create-fself" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
chmod +x "$OO/bin/create-fself"

for tool in clang-21 clang++-21 ld.lld-21 llvm-ar-21 llvm-ranlib-21 llvm-objcopy-21 ninja python3; do
    cat > "$TOOLS/$tool" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
    chmod +x "$TOOLS/$tool"
done
cat > "$TOOLS/cmake" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$CMAKE_ARGS_LOG"
printf 'CFLAGS=%s\n' "${CFLAGS-unset}" > "$CMAKE_ENV_LOG"
printf 'CXXFLAGS=%s\n' "${CXXFLAGS-unset}" >> "$CMAKE_ENV_LOG"
printf 'CPPFLAGS=%s\n' "${CPPFLAGS-unset}" >> "$CMAKE_ENV_LOG"
printf 'LDFLAGS=%s\n' "${LDFLAGS-unset}" >> "$CMAKE_ENV_LOG"
printf 'LIBS=%s\n' "${LIBS-unset}" >> "$CMAKE_ENV_LOG"
STUB
chmod +x "$TOOLS/cmake"

export OPENORBIS="$OO" PSB
export PATH="$TOOLS:$PATH"
export CMAKE_ARGS_LOG="$TMP/cmake.args" CMAKE_ENV_LOG="$TMP/cmake.env"
export CFLAGS=leak CXXFLAGS=leak CPPFLAGS=leak LDFLAGS=leak LIBS=leak

source "$KIT_ROOT/toolchain/env.sh"

[[ "$PS4_KIT" == "$KIT_ROOT" ]]
[[ "$PS4_BUILD_ROOT" == "$PSB" ]]
[[ "$PS4_SYSROOT" == "$PSB/sysroot" ]]
[[ "$PS4_MESA_SOURCE" == "$PSB/src/mesa" ]]
[[ "$PS4_MESA_BUILD" == "$PSB/build/mesa" ]]
[[ "$PS4_BASE_FLAGS" == *"--target=$PS4_TARGET"* ]]

ps4_cmake -S "$TMP/project" -B "$TMP/build"
grep -Fq -- "-DCMAKE_TOOLCHAIN_FILE=$OPENORBIS/cmake/ps4.cmake" "$CMAKE_ARGS_LOG"
grep -Fq -- "-DCMAKE_BUILD_TYPE=Release" "$CMAKE_ARGS_LOG"
grep -Fq 'CFLAGS=unset' "$CMAKE_ENV_LOG"
grep -Fq 'CXXFLAGS=unset' "$CMAKE_ENV_LOG"
grep -Fq 'CPPFLAGS=unset' "$CMAKE_ENV_LOG"
grep -Fq 'LDFLAGS=unset' "$CMAKE_ENV_LOG"
grep -Fq 'LIBS=unset' "$CMAKE_ENV_LOG"

echo "ps4-kit environment smoke test passed"
