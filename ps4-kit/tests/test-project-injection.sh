#!/usr/bin/env bash
# Host-only contract test for the two public CMake project-injection consumers:
# MGR-PS4 (project(sa2)) and MP3-PS4 (project(mp3)).
# This verifies CMake hook timing and the reconstructed kit's configure-time
# interface. It does NOT claim that either target links or runs on PS4.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIT_ROOT="$(cd "$HERE/.." && pwd)"
FIXTURE="$HERE/project-injection"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v cmake >/dev/null 2>&1 || { echo "missing host tool: cmake" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "missing host tool: python3" >&2; exit 1; }

OPENORBIS="$TMP/openorbis"
mkdir -p "$OPENORBIS/bin"
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

for project in sa2 mp3; do
    build="$TMP/build-$project"
    echo "== CMake project(${project}) injection"
    cmake -S "$FIXTURE" -B "$build" \
        "-DPS4_KIT_TEST_PROJECT=$project" \
        "-DCMAKE_PROJECT_${project}_INCLUDE=$KIT_ROOT/ps4-kit.cmake" \
        "-DOPENORBIS=$OPENORBIS" \
        -DPS4_KIT_NETFS=OFF \
        > "$TMP/$project.log" 2>&1 || {
            cat "$TMP/$project.log" >&2
            exit 1
        }
    grep -q "PS4 kit injection verified for ${project}" "$TMP/$project.log" || {
        cat "$TMP/$project.log" >&2
        echo "missing project-injection verification for $project" >&2
        exit 1
    }
    test -s "$build/ps4-link.x"
    grep -q 'SORT_BY_INIT_PRIORITY(.init_array' "$build/ps4-link.x"
    echo "PASS: $project project hook, runtime target, helper functions and linker script"
done

echo "PASS: MGR/MP3 host configure contract (no target compilation or runtime claim)"
