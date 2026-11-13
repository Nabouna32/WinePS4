#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
script="$ROOT/ps4-kit/scripts/build-mesa.sh"
bash -n "$script"
help="$(bash "$script" --help)"
grep -q "Usage: build-mesa.sh" <<< "$help"
grep -q "PS4_MESA_SOURCE" <<< "$help"
echo "Mesa build-script syntax/help tests passed"
