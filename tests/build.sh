#!/usr/bin/env bash
# Builds the Windows test programs with mingw. Run in WSL.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
x86_64-w64-mingw32-gcc -O1 -Wall -o "$HERE/hello.exe" "$HERE/hello.c"
ls -la "$HERE/hello.exe"
