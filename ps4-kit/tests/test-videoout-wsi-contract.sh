#!/usr/bin/env bash
# Static contract checks for the native VideoOut WSI and its separation from Wine.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NATIVE="$ROOT/ps4-kit/mesa/ps4_vk_wsi_videoout.cpp"
WINDOW="$ROOT/ps4-kit/mesa/ps4_vk_wsi_window.cpp"
CMAKE="$ROOT/toolchain/ps4-kit-compat/ps4-kit.cmake"

[ -f "$NATIVE" ]
[ -f "$WINDOW" ]

grep -q 'sceSysmoduleLoadModuleInternal' "$NATIVE"
grep -q 'ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE' "$NATIVE"
grep -q 'sceVideoOutOpen' "$NATIVE"
grep -q 'sceVideoOutRegisterBuffers' "$NATIVE"
grep -q 'sceVideoOutUnregisterBuffers' "$NATIVE"
grep -q 'sceVideoOutGetFlipStatus' "$NATIVE"
grep -q 'last_flip_arg' "$NATIVE"
grep -q 'wait_off_screen' "$NATIVE"
grep -q 'sceVideoOutSubmitFlip' "$NATIVE"
grep -q 'ac_ps4_submit_flip' "$NATIVE"
grep -q 'kFramebufferAlign' "$NATIVE"
grep -q 'g_retired' "$NATIVE"
grep -q 'wsi_queue_submit2' "$NATIVE"
grep -q 'VK_SEMAPHORE_TYPE_TIMELINE' "$NATIVE"
grep -q 'SceSysmodule SceSystemService SceVideoOut' "$CMAKE"

if grep -q 'sceVideoOut\|ac_ps4_submit_flip' "$WINDOW"; then
    echo "Wine window WSI must not own native VideoOut" >&2
    exit 1
fi
if grep -q 'ps4kit_window_present' "$NATIVE"; then
    echo "Native VideoOut WSI must not call Wine's desktop callback" >&2
    exit 1
fi

echo "PASS: native VideoOut WSI contract and profile separation"
