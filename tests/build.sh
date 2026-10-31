#!/usr/bin/env bash
# Builds the Windows test programs: hello.exe (64-bit, mingw) and hello32.exe (32-bit, with the
# WoW64 host build's winegcc and clang: there is no 32-bit mingw here). Run in WSL.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
x86_64-w64-mingw32-gcc -O1 -Wall -o "$HERE/hello.exe" "$HERE/hello.c"
"$HOME/wine-ps4-build/install/wine-wow64/bin/winegcc" -b i686-windows -mconsole -o "$HERE/hello32.exe" "$HERE/hello.c"
ls -la "$HERE/hello.exe" "$HERE/hello32.exe"
