# Wine window-mode Vulkan WSI contract

This note records the interface recovered from the checked-in WinePS4 patch and the
constraints for the clean-room implementation in `ps4-kit/mesa/ps4_vk_wsi_window.cpp`.
The implementation is an initial pass and remains subject to target validation.

## 1. The application-facing contract

The WinePS4 patch adds `dlls/wineps4.drv/vulkan.c`. Its `PS4DRV_VulkanInit()`
registers two callbacks with the kit:

- `ps4kit_set_window_present()`
- `ps4kit_set_log()`

The frame callback ABI is:

```c
void (*present)(unsigned long long surface, const void *pixels,
                unsigned width, unsigned height,
                unsigned row_bytes, int bgra);
```

The `surface` value is the opaque `VkSurfaceKHR` returned when Wine creates a
headless surface. Wine stores that exact value in its client-surface table and
uses it to find the corresponding HWND. Do not substitute a swapchain handle,
window handle, VideoOut buffer number, or process-global surface ID.

The callback is synchronous from the kit's point of view: Wine immediately passes
the pixel pointer and row pitch to `ps4drv_send_frame()`. The WSI must keep the
mapped image valid and unchanged until the callback returns. `row_bytes` is a
byte stride, not a pixel count. `bgra != 0` means BGRA byte order; zero means
RGBA byte order. The desktop driver clips the client image into its top-level
window and forwards the pixels to the desktop socket.

## 2. Required WSI behavior

The Wine process renders individual resizable windows. The desktop process owns
VideoOut. Therefore the WSI must be headless and window-sized:

- advertise `VK_EXT_headless_surface` and `VK_KHR_swapchain` through the
  Vulkan dispatch path used by Wine;
- create/destroy opaque headless `VkSurfaceKHR` handles;
- report presentation support only for queue families that can actually present;
- report formats whose byte order can be represented by the callback's `bgra`
  flag;
- permit application-selected swapchain extents (the HWND/client dimensions are
  not passed into the headless-surface create call);
- create swapchain images usable by the renderer and CPU-readable by the callback;
- expose each image's real `VkSubresourceLayout::rowPitch`;
- wait for rendering and presentation dependencies before reading pixels;
- call `ps4kit_window_present((unsigned long long)surface, pixels, width, height,
  row_bytes, bgra)` before releasing or reusing the mapped image;
- support swapchain destruction/recreation, including resize and
  `oldSwapchain` handling.

The initial correctness-first implementation submits an empty operation to the
present queue, waits on the application's presentation semaphores, signals a
fence, and waits for that fence before invoking the callback. This is slower
than a GPU-copy/async path, but avoids handing Wine a pointer to an image still
being written while preserving the caller's binary wait semaphores. Acquire
uses an empty submit to signal the requested semaphore/fence. Do not silently discard binary semaphores:
a separate public PS4 WSI does this because of a platform-specific failure mode,
but that behavior has not been established for this WinePS4/Mesa combination.

## 3. Memory requirements

The current implementation requires a memory type satisfying
`DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` for linear swapchain images. It uses
`vkGetImageSubresourceLayout()` for the mapped base offset and row pitch rather
than assuming `width * 4`. Non-coherent memory fallback (flush/invalidate) is not
implemented yet; the WSI must fail swapchain creation if no coherent host-visible
type is available.

The public SSBU WSI is useful evidence for PS4 RADV's linear host-visible image
allocation, but its fixed 1920x1080 VideoOut registration, VideoOut flips, and
full-screen lifecycle are incompatible with this contract. No part of that
full-screen path is copied into the Wine window-mode WSI.

## 4. Dispatch integration recovered

The route is explicitly present in the unchanged `port/ps4_unixlibs.c`:

- Wine registers a synthetic `libvulkan.so.1` in `ps4_unixlibs[]`;
- its `vkGetInstanceProcAddr` export calls `ps4kit_vk_get_instance_proc_addr()`;
- its `vkGetDeviceProcAddr` export obtains the kit's device-proc function from
  that same instance-proc entry point and forwards device lookups to it.

Therefore the kit WSI translation unit must export the C ABI symbol
`ps4kit_vk_get_instance_proc_addr(void)`, returning the kit-owned
`PFN_vkGetInstanceProcAddr`. It must provide a consistent device dispatch path
that keeps the swapchain/present interceptors reachable. This is a concrete
link-time contract, not an inferred loader hook. `PS4DRV_VulkanInit()`'s
`vulkan_handle` argument is unrelated to this synthetic library path and need
not be used for WSI dispatch.

The public SSBU port's `vk_get_instance_proc_addr()` is the reference for wrapping
RADV's `vk_icdGetInstanceProcAddr` and `vk_common_GetDeviceProcAddr`. This branch
exports Wine's required `ps4kit_vk_get_instance_proc_addr()` name and supplies
its own window-mode surface/swapchain dispatch. The CMake fail-closed guard stays
until target Mesa linking, actual queue synchronization, and window presentation
are validated.

## 5. Evidence and limits

- WinePS4 source: `patches/wine-ps4.patch`, new
  `dlls/wineps4.drv/vulkan.c` (the callback registration, client-surface map,
  headless-surface creation, and frame forwarding).
- WinePS4 build integration: `app/CMakeLists.txt`, which links RADV and calls
  `ps4_kit_link_mesa(wineps4)`.
- Public comparison only: `iHaiDeeZ/SSBU`,
  `port/ps4_vk_wsi.cpp` and `port/ps4_vk_wsi.h`, MIT-licensed in that
  repository. Its code is not a drop-in implementation and is not copied here.

A host C++ syntax check against the system Vulkan-Headers package passes in CI.
No PS4 target compilation, Vulkan conformance test, real window presentation, or
hardware test has been performed. The CMake WSI guard must remain enabled until
the exact target Mesa/Vulkan-Headers combination and window-mode behavior are validated.
