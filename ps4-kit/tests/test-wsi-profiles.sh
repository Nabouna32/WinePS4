#!/usr/bin/env bash
# Host-only regression test: Wine desktop WSI and native VideoOut WSI
# must never be silently interchanged by the shared Mesa/app helpers.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v cmake >/dev/null 2>&1
command -v python3 >/dev/null 2>&1
OPENORBIS="$TMP/openorbis"
MESA_BUILD="$TMP/mesa-build"
MESA_SOURCE="$TMP/mesa-source"
VULKAN_HEADERS="$TMP/vulkan-headers"
mkdir -p "$OPENORBIS/bin" "$MESA_SOURCE" "$VULKAN_HEADERS/vulkan"
: > "$OPENORBIS/bin/create-fself"
cat > "$OPENORBIS/link.x" <<'EOF'
SECTIONS
{
  .init_array : { *(.init_array); }
  .fini_array : { *(.fini_array) }
  .data : { *(.data) }
  .tdata : { *(.tdata) }
  .tbss : { *(.tbss) }
}
EOF
: > "$VULKAN_HEADERS/vulkan/vulkan.h"

while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    mkdir -p "$MESA_BUILD/$(dirname "$relative")"
    : > "$MESA_BUILD/$relative"
done <<'EOF'
src/amd/vulkan/libvulkan_radeon.a
src/amd/common/libamd_common.a
src/amd/compiler/libaco.a
src/amd/addrlib/libaddrlib.a
src/vulkan/util/libvulkan_util.a
src/compiler/spirv/libvtn.a
src/compiler/nir/libnir.a
src/compiler/libcompiler.a
src/util/libxmlconfig.a
src/util/libmesa_util.a
src/util/libmesa_util_simd.a
src/util/libmesa_util_clflush.a
src/util/libmesa_util_clflushopt.a
src/util/libparson.a
src/util/blake3/libblake3.a
src/c11/impl/libmesa_util_c11.a
EOF

NATIVE_WSI="$TMP/native-wsi.cpp"
: > "$NATIVE_WSI"
FIXTURE="$TMP/fixture"
mkdir -p "$FIXTURE"
cat > "$FIXTURE/main.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$FIXTURE/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project($ENV{TEST_PROJECT} C CXX)
include($ENV{PS4_KIT_ROOT}/ps4-kit.cmake)
add_executable(test_app main.c)
if (TEST_CALL_ORDER STREQUAL "app-first")
    ps4_kit_app(test_app)
    ps4_kit_link_mesa(test_app)
else ()
    ps4_kit_link_mesa(test_app)
    ps4_kit_app(test_app)
endif ()
get_target_property(_sources test_app SOURCES)
if (TEST_EXPECTED_WSI STREQUAL "window")
    list(FIND _sources "${PS4_KIT_WINDOW_WSI_SOURCE}" _window_index)
    list(FIND _sources "${PS4_KIT_NATIVE_WSI_SOURCE}" _native_index)
    if (_window_index EQUAL -1 OR NOT _native_index EQUAL -1)
        message(FATAL_ERROR "Wine profile did not attach only the window-desktop WSI: ${_sources}")
    endif ()
elseif (TEST_EXPECTED_WSI STREQUAL "native")
    list(FIND _sources "${PS4_KIT_NATIVE_WSI_SOURCE}" _native_index)
    list(FIND _sources "${PS4_KIT_WINDOW_WSI_SOURCE}" _window_index)
    if (_native_index EQUAL -1 OR NOT _window_index EQUAL -1)
        message(FATAL_ERROR "Native profile did not attach only the VideoOut WSI: ${_sources}")
    endif ()
else ()
    message(FATAL_ERROR "Unknown TEST_EXPECTED_WSI")
endif ()
message(STATUS "PASS: ${CMAKE_PROJECT_NAME} selected ${TEST_EXPECTED_WSI} WSI only")
EOF

for spec in "wineps4:window:app-first" "sa2:native:link-first"; do
    project="${spec%%:*}"
    rest="${spec#*:}"
    expected="${rest%%:*}"
    order="${rest##*:}"
    build="$TMP/build-$project"
    echo "== WSI profile: $project -> $expected"
    TEST_PROJECT="$project" \
    PS4_KIT_ROOT="$ROOT/ps4-kit" \
    cmake -S "$FIXTURE" -B "$build" \
        -DOPENORBIS="$OPENORBIS" \
        -DPS4_MESA_BUILD="$MESA_BUILD" \
        -DPS4_MESA_SOURCE="$MESA_SOURCE" \
        -DPS4_VULKAN_HEADERS="$VULKAN_HEADERS" \
        -DPS4_KIT_NATIVE_WSI_SOURCE="$NATIVE_WSI" \
        -DTEST_EXPECTED_WSI="$expected" \
        -DTEST_CALL_ORDER="$order" \
        > "$TMP/$project.log" 2>&1 || {
            cat "$TMP/$project.log" >&2
            exit 1
        }
    grep -q "PASS: $project selected $expected WSI only" "$TMP/$project.log"
done

echo "PASS: WSI profiles remain separate"
