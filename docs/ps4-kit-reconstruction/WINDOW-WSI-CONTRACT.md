# Wine window-mode Vulkan WSI contract

This note records the interface recovered from the checked-in WinePS4 patch and the
constraints for a clean-room implementation in `ps4-kit/mesa/`. It deliberately
does not claim that a WSI implementation exists yet.

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

The initial correctness-first implementation may wait for the relevant queue to
be idle before invoking the callback. This is expected to be slower than a GPU
copy/async path, but avoids handing Wine a pointer to an image still being
written. It must still correctly consume the present wait semaphores and honour
acquire semaphore/fence semantics. Do not silently discard binary semaphores:
a separate public PS4 WSI does this because of a platform-specific failure mode,
but that behavior has not been established for this WinePS4/Mesa combination.

## 3. Memory requirements

The candidate implementation should prefer a memory type satisfying
`HOST_VISIBLE | HOST_COHERENT` for linear swapchain images. It must use
`vkGetImageSubresourceLayout()` for the mapped base offset and row pitch rather
than assuming `width * 4`. If coherent memory is unavailable, the implementation
must flush/invalidate the correct mapped ranges as required by Vulkan before CPU
access. Any alignment padding must be included in the allocation and binding
calculation.

The public SSBU WSI is useful evidence for PS4 RADV's linear host-visible image
allocation, but its fixed 1920x1080 VideoOut registration, VideoOut flips, and
full-screen lifecycle are incompatible with this contract. No part of that
full-screen path is copied into the Wine window-mode WSI.

## 4. Dispatch integration is still unresolved

The WinePS4 patch's surface callback establishes the HWND-to-`VkSurfaceKHR`
mapping, but the patch excerpt does not by itself establish how calls such as
`vkCreateHeadlessSurfaceEXT`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`,
`vkCreateSwapchainKHR`, and `vkQueuePresentKHR` are routed to a kit-owned
implementation instead of RADV's default dispatch.

The public SSBU port solves this for DXVK by exposing a custom
`vk_get_instance_proc_addr()` and patching DXVK's loader to use it. WinePS4 has
no equivalent loader hook in the checked-in patch. Its
`PS4DRV_VulkanInit()` receives a `vulkan_handle` parameter but currently does
not use it. Before writing the WSI translation unit or removing the CMake
fail-closed guard, the actual Wine/Mesa dispatch route must be proven. Merely
adding a C++ source file to `wineps4` does not make Vulkan calls reach it.

## 5. Evidence and limits

- WinePS4 source: `patches/wine-ps4.patch`, new
  `dlls/wineps4.drv/vulkan.c` (the callback registration, client-surface map,
  headless-surface creation, and frame forwarding).
- WinePS4 build integration: `app/CMakeLists.txt`, which links RADV and calls
  `ps4_kit_link_mesa(wineps4)`.
- Public comparison only: `iHaiDeeZ/SSBU`,
  `port/ps4_vk_wsi.cpp` and `port/ps4_vk_wsi.h`, MIT-licensed in that
  repository. Its code is not a drop-in implementation and is not copied here.

No PS4 target compilation, Vulkan conformance test, real window presentation, or
hardware test has been performed. The CMake WSI guard must remain enabled until
both dispatch integration and the window-mode implementation are validated.
