#!/usr/bin/env bash
# Verify PS4_DIAG_DATA_DIR exports the same unmangled symbol from C and C++.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CC="${CC:-cc}"
CXX="${CXX:-c++}"
command -v "$CC" >/dev/null 2>&1
command -v "$CXX" >/dev/null 2>&1
command -v nm >/dev/null 2>&1

cat > "$TMP/test.c" <<'EOF'
#include "ps4_kit_diag.h"
PS4_DIAG_DATA_DIR("/data/PS4KitC");
int main(void) { return ps4_kit_diag_data_dir[0] == '/' ? 0 : 1; }
EOF
cat > "$TMP/test.cpp" <<'EOF'
#include "ps4_kit_diag.h"
PS4_DIAG_DATA_DIR("/data/PS4KitCXX");
int main() { return ps4_kit_diag_data_dir[0] == '/' ? 0 : 1; }
EOF

"$CC" -std=c11 -Wall -Wextra -Werror -fvisibility=hidden \
    -I"$ROOT/ps4-kit/include" "$TMP/test.c" -o "$TMP/test-c"
"$CXX" -std=c++17 -Wall -Wextra -Werror -fvisibility=hidden \
    -I"$ROOT/ps4-kit/include" "$TMP/test.cpp" -o "$TMP/test-cpp"

nm -g "$TMP/test-c" | grep -q ' ps4_kit_diag_data_dir$'
nm -g "$TMP/test-cpp" | grep -q ' ps4_kit_diag_data_dir$'
"$TMP/test-c"
"$TMP/test-cpp"
echo "PASS: PS4_DIAG_DATA_DIR exports a stable C symbol"
