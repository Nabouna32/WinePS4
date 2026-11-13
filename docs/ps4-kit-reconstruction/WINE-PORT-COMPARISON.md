# Independent Wine-on-PS4 comparison: orbis-win

Date: 2026-11-13

This is a separate PS4 Wine implementation by another author, not a source dependency of WinePS4 and not evidence that the private ps4-kit was copied from it. It is nevertheless a useful independent reference because it has reached real gameplay on hardware and publishes detailed Wine/Mesa engineering notes.

Repository: [plastikman/orbis-win](https://github.com/plastikman/orbis-win). It is a PS4 port of [prospero-win](https://github.com/plastikman/prospero-win), based on a different Wine runtime patch series (ABI-v4) and its own Mesa RADV patches. Its README reports real-console gameplay for several titles as of 2026-11-08. This is an independent engineering line, not a drop-in replacement.

## 1. Graphics architecture differs from WinePS4's private kit

orbis-win uses a `libvulkan.prx` bridge (`src/native/pw_vulkan_radv_ps4.c`) that exports the Vulkan loader's `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`, forwarding to Mesa's ICD symbols. Its PS4 Mesa WSI (`wsi_orbis`) owns native VideoOut scan-out. Wine calls exported presentation helpers for cursor and non-swapchain title frames.

Its Mesa patch 0039 fits arbitrary-size swapchains into the fixed 1920x1080 scan-out with aspect-preserving nearest-neighbour scaling and black bars. The project documents why this was necessary: win32u reports the actual window size, but VideoOut scan-out is fixed-size. This is a distinct design from WinePS4's private kit window-mode WSI:

- WinePS4: each program process has no VideoOut; RADV uses a headless surface and swapchain images in host-visible memory, then synchronously hands frames to the desktop callback (`set_window_present`, private kit prefix `195da48`).
- orbis-win: one native VideoOut scan-out is owned by the PS4 WSI; its Wine bridge sends cursor/title frames to the scan-out when a game swapchain is not presenting.

Do not transplant its WSI into WinePS4. It can help validate RADV loader dispatch, frame ownership, swapchain sizing, scan-out lifetime and diagnostics, but the process/window ownership contracts differ.

## 2. Useful independent runtime findings

The public [`pw_vulkan_radv_ps4.c`](https://github.com/plastikman/orbis-win/blob/47c7bd3dbf331f249a23bb2371b34616ab086fe7/src/native/pw_vulkan_radv_ps4.c) records several platform problems worth checking against WinePS4's own runtime:

- The OpenOrbis static libc gives each PRX its own `environ`; setting variables in another module does not necessarily publish them to RADV. The PRX publishes a valid environment before the driver is entered.
- RADV expects `program_invocation_short_name`, which was NULL in this environment; the PRX supplies a safe process name.
- The console's general-dynamic TLS path returned NULL for RADV's `__tls_get_addr`; the project supplies a working implementation for its PRX. This is a different case from WinePS4's statically linked Mesa and must be tested in the actual link model before borrowing the workaround.
- `strdup(NULL)` in the console libc faults rather than returning NULL; the PRX wraps it with a guard and diagnostic.
- The PS4 bridge exports small presentation helpers instead of exposing the WSI internals directly.

These are excellent independent diagnostic clues, not confirmed bugs in WinePS4's static whole-archive Mesa path. In particular, a PRX's private libc environment and TLS are not automatically equivalent to the eboot's statically linked environment.

## 3. Memory and GPU allocation evidence

orbis-win documents a different direct-memory allocator/VA strategy for RADV. Its handoff notes describe:
- a 1 GiB virtual-address window extension with direct memory committed on demand;
- honest heap accounting split between GTT and VRAM;
- retiring freed GPU VA ranges at each frame boundary, because deferred presentation means the GPU may not reach the newest submission and therefore an overly strict retire condition leaks address space;
- diagnostics for RADV allocation failures and memory exhaustion.

WinePS4's own private kit has its own separate placed-map milestones: Mesa patch 0030 (`941b771`) for fixed direct-memory aliases and patch 0031 (`53cf427`) to enable direct-memory aliasing. The independent project is useful for comparing the general problem of VA lifetime and GPU memory pressure, but its allocation policy and WSI must not be substituted without checking the Wine kit's exact Mesa branch and memory ownership.

## 4. What to mine next

1. Compare the orbis-win RADV loader shim with WinePS4's `ps4_unixlibs.c` and `ps4kit_vk_get_instance_proc_addr()` contract.
2. Read its Mesa allocation/retire patches as an independent review of placed-map and GPU-VA lifetime correctness.
3. Compare the TLS/environment workarounds only after establishing whether the relevant WinePS4 object is in the eboot or a separate PRX.
4. Keep the existing Wine window callback and process roles unchanged. orbis-win is a reference for tests and failure modes, not a reason to change WinePS4's architecture.

Sources:
- [orbis-win README](https://github.com/plastikman/orbis-win)
- [orbis-win findings](https://github.com/plastikman/orbis-win/blob/47c7bd3dbf331f249a23bb2371b34616ab086fe7/docs/FINDINGS.md)
- [orbis-win PS4 Vulkan bridge](https://github.com/plastikman/orbis-win/blob/47c7bd3dbf331f249a23bb2371b34616ab086fe7/src/native/pw_vulkan_radv_ps4.c)
- [orbis-win Mesa WSI patch 0039](https://github.com/plastikman/orbis-win/blob/47c7bd3dbf331f249a23bb2371b34616ab086fe7/src/mesa-patches-ps4/0039-wsi-orbis-fit-a-swapchain-of-any-size-to-the-screen.patch)
- [orbis-win latest handoff](https://github.com/plastikman/orbis-win/blob/47c7bd3dbf331f249a23bb2371b34616ab086fe7/docs/HANDOFF-2026-11-08.md)
