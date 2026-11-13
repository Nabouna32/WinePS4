# RADV / Mesa build-link trace

Status: initial Wine window-mode WSI and the public PS4 GNM winsys source are now present in the reconstruction branch. Target Mesa integration/build and PS4 validation remain incomplete.

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

Source: https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/toolchain/ps4-love-style.cmake

## Public evidence: DolphinPS4 Mesa build script

The public `scripts/build-mesa.sh` independently lists a broader static target set, including separate archives for:

- `src/vulkan/runtime/libvulkan_instance.a`
- `src/vulkan/runtime/libvulkan_runtime.a`
- `src/vulkan/wsi/libvulkan_wsi.a`)

It also describes a PS4 GNM winsys based on `libSceGnmDriver`, with no Linux DRM device on the console. It stages a PS4 GNM winsys `ac_ps4_drm.c` into Mesa's AMD common sources.

Source: https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/scripts/build-mesa.sh

The MIT-licensed GNM winsys source from that exact DolphinPS4 commit is now included at
`ps4-kit/mesa/ac_ps4_drm.c`. Its original SPDX notice and copyright line are retained. The
source's GitHub blob is `a4589f0601c889a200a93d59968c763daff4aff2` (2,938 lines). The guarded
`ps4-kit/mesa/prepare-source.sh` stages it into an already-patched Mesa tree only if Meson's
`src/amd/common/meson.build` already lists `ac_ps4_drm.c`; it refuses to invent or rewrite the
revision-sensitive Meson integration.

## Public evidence: xemu-ps4 and MGR-PS4

- xemu-ps4 identifies `port/mesa/ac_ps4_drm.c` as the PS4 RADV GNM winsys and says flips go through GNM + VideoOut: https://github.com/iHaiDeeZ/xemu-ps4/blob/main/docs/PORTING.md
- MGR-PS4 says its renderer uses Mesa RADV plus the kit's VideoOut swapchain, with `ps4kit::vk_get_instance_proc_addr`: https://github.com/iHaiDeeZ/MGR-PS4/blob/main/docs/DEVELOPMENT.md

These are related but not identical layers: the Mesa GNM winsys submits GPU work; the ps4-kit VideoOut WSI presents frames.

## Build targets versus final link closure

The two Dolphin lists describe different stages, not necessarily conflicting dependencies. The Meson build script lists all static targets produced by the build, including standalone Vulkan runtime/WSI archives. The matching CMake helper links the final whole-archive closure and explicitly comments that `libvulkan_radeon.a` already contains the Vulkan runtime and WSI objects, so it does not link those standalone targets a second time. This is a coherent candidate closure for that Mesa build.

For WinePS4's target kit revision `56b3d86`, the exact Mesa revision and archive closure are still not known. Do not replace the current deliberate CMake failure with the public list until the Mesa source/patch version is selected and the archive members are verified.

## Next verification steps

1. Recover the Mesa source revision and Meson configuration used by WinePS4's ps4-kit `56b3d86`, if any public dependent commit or build trace records it.
2. Verify the archive members and undefined symbols for the selected Mesa build against the public helper's stated `libvulkan_radeon.a` runtime/WSI aggregation.
3. Identify the exact symbols WinePS4's final link expects from ps4-kit (including any `ps4kit::` entry points).
4. Separate RADV/GNM winsys symbols from VideoOut WSI symbols and from generic OpenOrbis runtime hooks.
5. Pin/reconstruct the Mesa Meson integration patch, stage the MIT GNM winsys, and build the archive closure; then validate the clean-room WSI against that exact Mesa revision.

## Scope reminder

This work is justified only because `ps4_kit_link_mesa(wineps4)` is a direct blocker to compiling the current application. Do not expand into graphics performance or new rendering features.

## Additional public VideoOut WSI implementation found

A later public port provides a concrete, licensed implementation candidate rather than only an API description:

- [SSBU `port/ps4_vk_wsi.cpp`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.cpp)
- [SSBU `port/ps4_vk_wsi.h`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.h)
- [SSBU MIT licence](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/LICENSE)

The implementation wraps statically linked RADV's ICD/device proc-address functions, exposes `VK_EXT_headless_surface` and `VK_KHR_swapchain`, allocates linear host-visible images, registers their addresses with VideoOut, and submits flips through the Mesa PS4 winsys. Its exported entry point is `dxvk_ps4_vk_get_instance_proc_addr()`; other consumers describe the same conceptual boundary as `ps4kit::vk_get_instance_proc_addr`.

This provides the public dispatch and linear-image evidence used by the clean-room Wine WSI in `ps4-kit/mesa/ps4_vk_wsi_window.cpp`. The SSBU implementation itself is not copied into the kit: its VideoOut presentation and SSBU-specific behavior are excluded.

SSBU's [Mesa build script](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/scripts/build-mesa.sh) also pins a Crema Mesa base commit (`7d010de97ed0e5d3b02e0d7c6bf0c829298490c9`) and builds the archive set in its CMake helper. This is a stronger reproducible candidate than a floating Mesa branch, but it is later than WinePS4's kit pin and must be treated as a candidate baseline, not as proof of the original kit's exact Mesa revision.

## WinePS4's actual WSI contract (from its checked-in Wine patch)

The Wine patch narrows the interface considerably. Its `dlls/wineps4.drv/vulkan.c` does **not** ask the kit to present the entire process directly to VideoOut. It creates a headless host surface for each Wine client surface, then expects the kit's window-mode swapchain to call back with the completed pixels:

- `ps4kit_set_window_present(void (*hook)(unsigned long long surface, const void *pixels, unsigned width, unsigned height, unsigned row_bytes, int bgra))`
- `ps4kit_set_log(void (*log)(const char *line))`

The callback maps the host surface handle back to the Wine HWND, clips the rendered client area into the top-level window and sends the pixels to the desktop over WinePS4's existing display socket. The patch comments identify the implementation as `gpu/ps4_vk_wsi.cpp` and explicitly describe its swapchain images as window-sized, CPU-readable memory. The desktop itself owns VideoOut.

This means the SSBU WSI is useful Vulkan reference code, but **not a drop-in replacement**: it presents full-screen buffers to VideoOut, whereas WinePS4 needs a window-sized surface and a pixel callback. The reconstruction must implement the Wine-specific window mode and those two C ABI hooks while leaving Wine's existing callback/display behavior unchanged. This is now the precise graphics integration target.

## Concrete WSI adaptation map

The SSBU WSI source exposes the Vulkan entry points needed for a first adaptation: headless-surface creation/destruction, surface capability/format/present-mode queries, swapchain creation/destruction, image enumeration, image acquisition, queue presentation, and the `QueueSubmit` / `QueueSubmit2` semaphore wrappers.

Its current implementation is specifically full-screen:
- it fixes the surface extent to 1920x1080;
- allocates linear host-visible images and maps them;
- registers the image addresses with `sceVideoOutRegisterBuffers`;
- presents with `ac_ps4_submit_flip` or `sceVideoOutSubmitFlip`.

For Wine's window mode, the adaptation boundary is now explicit:
1. Keep the RADV ICD dispatch wrapping and the generic Vulkan surface/swapchain entry points.
2. Take the actual per-window extent and format from `VkSwapchainCreateInfoKHR`, rather than fixed screen dimensions.
3. Allocate CPU-readable linear swapchain images and retain each image's mapped pointer and `VkSubresourceLayout.rowPitch`.
4. In `QueuePresentKHR`, wait until the submitted GPU work is complete, then call `ps4kit_window_present(surface, pixels, width, height, row_bytes, bgra)`.
5. Do not call VideoOut from this WSI. Wine's existing `present_frame()` callback maps the surface to its HWND, clips the client area and sends the pixels to the desktop, which is the only VideoOut owner.
6. Preserve correct acquire/present synchronization, swapchain resize/destruction, BGRA/RGBA selection and image lifetime. SSBU's binary-semaphore workaround is PS4-specific and must be checked against Wine's DXVK 3.1.1 usage rather than copied blindly.

The clean-room runtime provides the callback-registration functions used by Wine (`ps4kit_set_window_present`, `ps4kit_set_log`) and the WSI exports `ps4kit_vk_get_instance_proc_addr()`. The new WSI implements surface/swapchain dispatch and uses a queue-submit fence wait before calling `ps4kit_window_present()` with the actual surface handle, mapped pixels, row pitch and format. Host syntax and fake-RADV dispatch tests pass in CI; target Mesa linking and hardware presentation remain unverified.

## Newer reproducible Mesa build-root evidence

DolphinPS4's pinned public build instructions identify **Mesa 26.0.8** in `$PS4_BUILD_ROOT/src/mesa`, the target sysroot in `$PS4_BUILD_ROOT/sysroot`, and the Meson build in `$PS4_BUILD_ROOT/build/mesa`. Its build script uses a PS4 cross file targeting `x86_64-pc-freebsd12-elf`, header-only libdrm pkg-config metadata (there is no Linux DRM device on PS4), a host `glslangValidator`, and static RADV archives.

References:
- [DolphinPS4 BUILDING.md](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/BUILDING.md)
- [DolphinPS4 build-mesa.sh](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/scripts/build-mesa.sh)
- [DolphinPS4 ps4-love-style.cmake](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/toolchain/ps4-love-style.cmake)
- [DolphinPS4 CREDITS](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/CREDITS.md), which identifies the PS4 Mesa GNM backend and patch as MIT/per-file licensed.

The reconstructed `ps4-kit/toolchain/env.sh` exports the same build-root vocabulary: `PS4_BUILD_ROOT`, `PS4_MESA_SOURCE`, and `PS4_MESA_BUILD`, defaulting to the existing WinePS4 build tree. The GNM winsys source is now vendored and can be staged into a pre-patched Mesa tree. No Mesa source is cloned or built yet because the exact Meson integration patch/revision remains unresolved.

**Important:** the public helper's whole-archive list omits the separate runtime/WSI archives that the public build script also builds. We must establish which archive set belongs to which Mesa patch/revision before using it. Mesa 26.0.8 is a promising later baseline, not a recovered fact about private kit revision `56b3d86`.
