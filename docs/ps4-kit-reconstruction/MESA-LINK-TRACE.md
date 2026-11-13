# RADV / Mesa build-link trace

Status: evidence comparison only. This is a build dependency investigation, not a working WinePS4 graphics port.

## Why this is a compile blocker

WinePS4's `app/CMakeLists.txt` calls `ps4_kit_link_mesa(wineps4)` after linking the `winevulkan` Unix module. That function must make the Vulkan implementation's static symbol closure available to the final PS4 executable.

The kit's graphics dependency is larger than "link RADV": MGR's public development notes identify both Mesa RADV and a ps4-kit VideoOut swapchain. MGR also describes a headless Vulkan surface and `ps4kit::vk_get_instance_proc_addr`. The window-system bridge is therefore part of the private build/runtime surface.

## Public evidence: DolphinPS4

The public `toolchain/ps4-love-style.cmake` implements a `ps4_link_mesa(target)` helper. It links these archives with `--whole-archive`:

- `src/amd/vulkan/libvulkan_radeon.a`
- `src/amd/common/libamd_common.a`
- `src/amd/compiler/libaco.a`
- `src/amd/addrlib/libaddrlib.a`
- `src/vulkan/util/libvulkan_util.a`
- `src/compiler/spirv/libvtn.a`
- `src/compiler/nir/libnir.a`
- `src/compiler/libcompiler.a`
- Mesa util, SIMD, XML config, Parson, BLAKE3 and C11 utility archives

The helper comments that the RADV archive already contains the Vulkan runtime and WSI objects, and that weak dispatch-table references require whole-archive linking.

Source: https://github.com/iHaiDeeZ/DolphinPS4/blob/main/toolchain/ps4-love-style.cmake

## Public evidence: DolphinPS4 Mesa build script

The public `scripts/build-mesa.sh` independently lists a broader static target set, including separate archives for:

- `src/vulkan/runtime/libvulkan_instance.a`
- `src/vulkan/runtime/libvulkan_runtime.a`
- `src/vulkan/wsi/libvulkan_wsi.a)

It also describes a PS4 GNM winsys based on `libSceGnmDriver`, with no Linux DRM device on the console. It builds a patched `ac_ps4_drm.c` into Mesa's AMD common sources.

Source: https://github.com/iHaiDeeZ/DolphinPS4/blob/main/scripts/build-mesa.sh

## Public evidence: xemu-ps4 and MGR-PS4

- xemu-ps4 identifies `port/mesa/ac_ps4_drm.c` as the PS4 RADV GNM winsys and says flips go through GNM + VideoOut: https://github.com/iHaiDeeZ/xemu-ps4/blob/main/docs/PORTING.md
- MGR-PS4 says its renderer uses Mesa RADV plus the kit's VideoOut swapchain, with `ps4kit::vk_get_instance_proc_addr`: https://github.com/iHaiDeeZ/MGR-PS4/blob/main/docs/DEVELOPMENT.md

These are related but not identical layers: the Mesa GNM winsys submits GPU work; the ps4-kit VideoOut WSI presents frames.

## Important discrepancy — do not copy an archive list blindly

Dolphin's CMake helper omits the separate runtime/WSI archives that its Mesa build script lists. This may reflect a later Mesa build layout, archive merging, or a mismatch between revisions. The helper and build script must be tied to the same Mesa commit before the list is considered authoritative.

For WinePS4's target kit revision `56b3d86`, the exact Mesa revision and archive closure are not yet known. Do not replace the current deliberate CMake failure with an unverified list.

## Next verification steps

1. Recover the Mesa source revision and Meson configuration used by WinePS4's ps4-kit `56b3d86`, if any public dependent commit or build trace records it.
2. Compare archive members and undefined symbols for the Mesa build used by DolphinPS4; verify whether WSI/runtime are merged into `libvulkan_radeon.a` or supplied separately.
3. Identify the exact symbols WinePS4's final link expects from ps4-kit (including any `ps4kit::` entry points).
4. Separate RADV/GNM winsys symbols from VideoOut WSI symbols and from generic OpenOrbis runtime hooks.
5. Only then implement a CMake helper and independent WSI needed by the current WinePS4 source.

## Scope reminder

This work is justified only because `ps4_kit_link_mesa(wineps4)` is a direct blocker to compiling the current application. Do not expand into graphics performance or new rendering features.

## Additional public VideoOut WSI implementation found

A later public port provides a concrete, licensed implementation candidate rather than only an API description:

- [SSBU `port/ps4_vk_wsi.cpp`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.cpp)
- [SSBU `port/ps4_vk_wsi.h`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.h)
- [SSBU MIT licence](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/LICENSE)

The implementation wraps statically linked RADV's ICD/device proc-address functions, exposes `VK_EXT_headless_surface` and `VK_KHR_swapchain`, allocates linear host-visible images, registers their addresses with VideoOut, and submits flips through the Mesa PS4 winsys. Its exported entry point is `dxvk_ps4_vk_get_instance_proc_addr()`; other consumers describe the same conceptual boundary as `ps4kit::vk_get_instance_proc_addr`.

This gives us an implementation to study and potentially adapt under its MIT terms, preserving the copyright and licence notice. It is not yet known whether WinePS4's private kit revision `56b3d86` used the same ABI, and the WSI currently has SSBU-specific logging/trace hooks and assumptions. Do not copy it into the kit unchanged or claim Wine compatibility until its API is reconciled with Wine's `winevulkan` path.

SSBU's [Mesa build script](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/scripts/build-mesa.sh) also pins a Crema Mesa base commit (`7d010de97ed0e5d3b02e0d7c6bf0c829298490c9`) and builds the archive set in its CMake helper. This is a stronger reproducible candidate than a floating Mesa branch, but it is later than WinePS4's kit pin and must be treated as a candidate baseline, not as proof of the original kit's exact Mesa revision.
