# Vulkan WSI source provenance and adaptation notes

This document names the public source files used to recover the missing kit interfaces. It distinguishes reference material from code copied into this branch.

## 1. WinePS4 contracts (the project being reconstructed)

- `port/ps4_unixlibs.c` in this fork exports a synthetic `libvulkan.so.1`. Its `vkGetInstanceProcAddr` wrapper calls `ps4kit_vk_get_instance_proc_addr()`; its `vkGetDeviceProcAddr` wrapper obtains the kit's device-proc function from that entry point. This is the exact ABI that the reconstructed kit implements.
  - [WinePS4 source on the reconstruction branch](https://github.com/Nabouna32/WinePS4/blob/research/ps4-kit-reconstruction/port/ps4_unixlibs.c)
- `patches/wine-ps4.patch` contains the Wine-side `PS4DRV_VulkanInit()` callback registration and the headless-surface / frame-presentation path. It establishes that the callback's opaque surface value is the `VkSurfaceKHR` used by Wine's client-window lookup.
  - [WinePS4 patch on the reconstruction branch](https://github.com/Nabouna32/WinePS4/blob/research/ps4-kit-reconstruction/patches/wine-ps4.patch)
- `app/CMakeLists.txt` calls `ps4_kit_link_mesa(wineps4)`, and therefore the WSI source is compiled into the existing app target without changing the app's source code.
  - [WinePS4 app CMake file](https://github.com/Nabouna32/WinePS4/blob/research/ps4-kit-reconstruction/app/CMakeLists.txt)

## 2. Public RADV/PS4 implementation references

Primary comparison:
- Repository: [iHaiDeeZ/SSBU](https://github.com/iHaiDeeZ/SSBU)
- Inspected commit: `7e83c0eb9367c657960876f1ea2b4b5491d99099`
- WSI source: [`port/ps4_vk_wsi.cpp`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.cpp)
- Header: [`port/ps4_vk_wsi.h`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.h)
- RADV GNM winsys: [`port/mesa/ac_ps4_drm.c`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/mesa/ac_ps4_drm.c)
- License: [`LICENSE`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/LICENSE) is MIT for that repository's own code. Its [`CREDITS.md`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/CREDITS.md) identifies Mesa/RADV as MIT and records the shared WSI lineage (SMW, Crema, Resonance).

What is reused as *technical evidence*:
- RADV's internal ICD entry points: `vk_icdGetInstanceProcAddr` and `vk_common_GetDeviceProcAddr`.
- The WSI dispatch pattern: expose a kit-owned `vkGetInstanceProcAddr`, intercept surface/swapchain calls, and forward unrelated calls into RADV.
- Linear `VK_IMAGE_TILING_LINEAR` swapchain images backed by host-visible/coherent memory.
- Query `VkSubresourceLayout` instead of assuming a tightly packed `width * 4` stride.

What is intentionally **not** reused:
- The SSBU WSI's VideoOut lifecycle, fixed 1920x1080 surface, VideoOut buffer registration, flip queue, flip submissions, splash-screen handling, and its binary-semaphore dropping workaround. Those are full-screen game-specific behavior and are incompatible with Wine's per-window desktop callback.
- The new `ps4-kit/mesa/ps4_vk_wsi_window.cpp` is a clean-room implementation of the Wine callback contract. It is not a copy of the SSBU source file. The MIT header here describes this branch's implementation; it does not relicense Mesa, Vulkan-Headers, Wine, or OpenOrbis.

## 3. Mesa GNM winsys source

The actual PS4 kernel/command-submission backend needed by RADV is now included as
`ps4-kit/mesa/ac_ps4_drm.c`.

- Upstream: [DolphinPS4 `port/mesa/ac_ps4_drm.c`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/mesa/ac_ps4_drm.c)
- Repository commit: `879b3e57f3d3180bcb5f2234db3903688b28cd7d`
- Upstream file blob: `a4589f0601c889a200a93d59968c763daff4aff2`
- File header: `Copyright 2026 DolphinPS4. SPDX-License-Identifier: MIT`
- Size at that revision: 2,938 lines.

This is the PS4 implementation of Mesa's `ac_linux_drm.h` interface. It maps GPU buffers into
the shared CPU/GPU address space, submits command buffers through GNM, and implements fences and
sync objects using the PS4 submission sequence. The copied file retains its upstream MIT SPDX notice, and `ps4-kit/mesa/LICENSE.ac_ps4_drm` carries the corresponding license text.

The companion public integration patch is
[DolphinPS4 `patches/mesa-ps4.patch`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/patches/mesa-ps4.patch)
(commit `879b3e57f3d3180bcb5f2234db3903688b28cd7d`, blob
`888487895341980d27314abf7cbbe403f3e587a3`, 1,265 lines). DolphinPS4's
[`CREDITS.md`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/CREDITS.md)
explicitly identifies Mesa 26.0.8 and the PS4 Mesa port/patch as MIT and per-file licensed.
The patch is included at `ps4-kit/mesa/patches/mesa-ps4.patch` with a separate MIT notice.
`prepare-source.sh` checks it with `git apply --check`, applies it only when it matches the
external Mesa checkout, then stages the GNM winsys. It does not attempt to adapt the patch to an
unmatched Mesa revision.

The companion public recipe is
[DolphinPS4 `scripts/build-mesa.sh`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/scripts/build-mesa.sh)
and its [Mesa build guide](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/BUILDING.md).
That guide identifies Mesa 26.0.8, the PS4 cross-file, header-only libdrm pkg-config shim and the
static RADV archive build. A clean-room adaptation is now included as
`ps4-kit/scripts/build-mesa.sh`; it builds the public 19-target set, while WinePS4's final link
uses the 16-archive closure in `ps4-kit/mesa/archives.cmake`.

## 4. Other public build references

- [iHaiDeeZ/SMW `docs/BUILDING.md`](https://github.com/iHaiDeeZ/SMW/blob/main/docs/BUILDING.md) describes a public PS4 RADV build based on Mesa 26.0.8, Vulkan-Headers in the build sysroot, clang 21, and the PS4 GNM winsys. It is a later public baseline, not proof of the exact private kit revision pinned by WinePS4.
- [DolphinPS4's public Mesa link helper](https://github.com/iHaiDeeZ/DolphinPS4/blob/main/toolchain/ps4-love-style.cmake) is the source for the candidate static archive closure in `ps4-kit/mesa/archives.cmake`. This archive list remains revision-sensitive.

## 5. Current implementation and validation boundary

`ps4-kit/mesa/ps4_vk_wsi_window.cpp` now implements the first clean-room WSI pass:

- Wine's synthetic Vulkan entry point;
- extension enumeration and stripping before forwarding to RADV;
- `VK_KHR_get_surface_capabilities2`-style surface capabilities/formats2 query interception when the ICD exposes those entry points;
- headless surfaces and per-window surface capabilities;
- linear host-visible/coherent swapchain images;
- image-layout row pitch and RGBA/BGRA callback metadata;
- acquire semaphore/fence signaling;
- a queue submit + fence wait before synchronous frame delivery, preserving the caller's binary wait semaphores;
- swapchain destruction and device/instance dispatch forwarding.

This is an **initial implementation**, not yet a validated WSI. Host syntax CI, a fake-RADV dispatch-forwarding test, the Mesa build-driver syntax/help test, and the guarded GNM/patch staging tests pass in GitHub Actions run [#244](https://github.com/Nabouna32/WinePS4/actions/runs/43500846603). Target compilation against the pinned Mesa/Vulkan-Headers set, validation-layer testing, resize/recreation testing, and real PS4 window presentation remain required. The CMake guard must remain until those checks pass. No WinePS4 project source paths are modified.


## 6. Native VideoOut WSI: separate implementation track

The public native WSI lineage is now traced more precisely:

- [SSBU native WSI](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.cpp), with header [ps4_vk_wsi.h](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.h).
- The source header identifies its implementation ancestry as SMW's VideoOut WSI, itself from Resonance's ps4_wsi.cpp and Crema's PS4WSI.cpp.
- [M33 technical guide](https://github.com/iHaiDeeZ/M33/blob/main/docs/TECHNICAL.md) independently describes the same architecture and names its implementation as a VideoOut WSI based on Resonance.
- [SMW build guide](https://github.com/iHaiDeeZ/SMW/blob/main/docs/BUILDING.md) documents linear 1920x1080 images flipped GPU-side through VideoOut.

The SSBU repository's own code is MIT (see its LICENSE and CREDITS: the PS4 port is credited to ShiroKlein). The source file has no separate SPDX header, so any adaptation must preserve the repository's MIT notice and retain the explicit SMW/Resonance/Crema lineage comments. This is distinct from the Mesa GNM winsys file, which has its own MIT per-file notice. Do not infer the license of a source snapshot solely from the project name; keep both attributions.

### Native WSI behavior observed in SSBU

- Exposes VK_EXT_headless_surface and VK_KHR_swapchain; RADV itself has no WSI.
- Opens VideoOut on the main bus and creates a kernel event queue for flip completion.
- Uses a fixed 1920x1080 output, 2–3 linear RGBA8 images and 2 MiB VideoOut framebuffer alignment.
- Uses vkGetImageSubresourceLayout() for row pitch and offset, allocates host-visible/coherent/device-local image memory, maps it, and registers the aligned addresses with sceVideoOutRegisterBuffers().
- Alternates VideoOut buffer-index ranges when a new swapchain is created, so the old set can remain on screen until the new one is registered.
- Tracks flip completion through sceVideoOutGetFlipStatus() and the event queue before reusing a buffer.
- Queues GPU-side flips through ac_ps4_submit_flip() behind the frame's GPU commands; the fallback waits for the queue and calls sceVideoOutSubmitFlip().
- Hides the system splash after the first successful present.
- Filters binary semaphores from submits but preserves timeline semaphores. The comments attribute the binary-semaphore/device-loss behavior to Crema and Resonance; treat it as a workload-specific workaround, not a generic Vulkan rule.

### Risks to resolve before accepting this as the shared implementation

1. The SSBU source is a useful complete reference, but it contains project-specific logging/boot tracing and fixed screen constants. The kit implementation needs a generic logging hook and an explicit output profile.
2. Its free_swapchain() unregisters VideoOut buffers and frees Vulkan images/memory; the kit port must explicitly audit map/unmap and failure rollback for every partially created image.
3. It assumes the graphics queue family is family 0 and that there is one in-order queue. Verify this against the exact RADV/GNM build and every target project.
4. It reports one RGBA8 surface format and a fixed extent. Add VK_KHR_get_surface_capabilities2 / formats2 only where the consumer and driver require them; do not advertise unsupported extensions.
5. Binary semaphore filtering is risky for multiple queues or apps with independent synchronization. Validate against the target Vulkan workloads before enabling it by default.
6. It uses a 2 MiB framebuffer alignment. Check the final mapped address including VkSubresourceLayout::offset, not just the allocation base.
7. Recreate/destroy must wait for all pending flips and unregister the old buffer set before releasing any backing memory.

### CMake state after the profile split

The clean-room CMake layer now has this contract:

- ps4_kit_link_mesa(target) links the generic RADV archive closure only.
- ps4_kit_app(target) selects window-desktop for the WinePS4 top-level project and native-videoout for other Mesa-linked apps, unless PS4_KIT_WSI_PROFILE explicitly overrides it.
- window-desktop attaches only ps4_vk_wsi_window.cpp.
- native-videoout expects ps4_vk_wsi_videoout.cpp. That source is not yet present; configuration fails explicitly rather than silently attaching Wine's window WSI.
- ps4-kit/tests/test-wsi-profiles.sh and the CI workflow verify that the two profiles attach different source files.

This makes the current compatibility boundary honest: Wine's profile remains isolated; MGR, MP3, TTT2, ConkerLR and Godot are identified as native-VideoOut consumers but cannot yet complete their eboot configure/link until the native WSI source and its dependencies are reconstructed.
