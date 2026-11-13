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

## 3. Other public build references

- [iHaiDeeZ/SMW `docs/BUILDING.md`](https://github.com/iHaiDeeZ/SMW/blob/main/docs/BUILDING.md) describes a public PS4 RADV build based on Mesa 26.0.8, Vulkan-Headers in the build sysroot, clang 21, and the PS4 GNM winsys. It is a later public baseline, not proof of the exact private kit revision pinned by WinePS4.
- [DolphinPS4's public Mesa link helper](https://github.com/iHaiDeeZ/DolphinPS4/blob/main/toolchain/ps4-love-style.cmake) is the source for the candidate static archive closure in `ps4-kit/mesa/archives.cmake`. This archive list remains revision-sensitive.

## 4. Current implementation and validation boundary

`ps4-kit/mesa/ps4_vk_wsi_window.cpp` now implements the first clean-room WSI pass:

- Wine's synthetic Vulkan entry point;
- extension enumeration and stripping before forwarding to RADV;
- headless surfaces and per-window surface capabilities;
- linear host-visible/coherent swapchain images;
- image-layout row pitch and RGBA/BGRA callback metadata;
- acquire semaphore/fence signaling;
- a queue submit + fence wait before synchronous frame delivery, preserving the caller's binary wait semaphores;
- swapchain destruction and device/instance dispatch forwarding.

This is an **initial implementation**, not yet a validated WSI. Host syntax CI, target compilation against the pinned Mesa/Vulkan-Headers set, validation-layer testing, resize/recreation testing, and real PS4 window presentation remain required. The CMake guard must remain until those checks pass. No WinePS4 project source paths are modified.
