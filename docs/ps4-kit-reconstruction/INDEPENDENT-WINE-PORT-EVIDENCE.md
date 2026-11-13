# Independent Wine-on-Orbis evidence: OrbisDesk

Date: 2026-11-13

This audit adds [jl-casella/OrbisDesk](https://github.com/jl-casella/OrbisDesk) as an independent technical reference. Its repository describes an experimental PS4 desktop based on Wine, WoW64 and native graphics, derived from prospero-win. Its hardware notes contain unusually useful evidence for the exact boundaries that WinePS4's private kit had to solve.

This is a comparison source, not a dependency to copy wholesale. OrbisDesk has its own source provenance and licence boundaries; each file must be checked before reuse.

## 1. Strong independent confirmation: placed Vulkan memory is essential for 32-bit guests

OrbisDesk's [HARDWARE_W124.md](https://github.com/jl-casella/OrbisDesk/blob/dev/docs/HARDWARE_W124.md) records a physical PS4 test:

- DXVK x64 creates a Vulkan device and reaches first presentation.
- DXVK x86 reaches device creation but fails in vkMapMemory2KHR / vkMapMemory2EXT with VK_ERROR_MEMORY_MAP_FAILED.
- The recorded cause is the PS4 branch of Crema's RADV winsys returning NULL for use_fixed_addr / VK_EXT_map_memory_placed. A 32-bit process needs CPU-visible mappings below 4 GiB.

The subsequent W126 note records the implemented fix and a physical test:

- Remap the BO's direct-memory backing at the address reserved by Wine using sceKernelMapDirectMemory with MAP_FIXED.
- Use 16 KiB alignment and report 16 KiB as minPlacedMemoryMapAlignment.
- Keep the GPU mapping; the CPU mapping aliases the same physical pages.
- Return the virtual reservation to Wine when unmapping.
- W126 reports 32-bit DXVK D3D9 success, with placed mappings below 4 GiB and a successful process exit.

This independently corroborates the WinePS4 engineering notes for private kit 941b771 / Mesa patch 0030 (placed mappings at the application's requested address) and 53cf427 / patch 0031 (enable direct-memory aliasing before the first placed map). It also clarifies that the requirement is not merely "a fixed mmap": the CPU alias must refer to the same direct-memory backing used by the GPU BO, and the mapping/unmapping lifecycle must preserve Wine's reservation.

Reference: [OrbisDesk W124/W126 hardware notes](https://github.com/jl-casella/OrbisDesk/blob/dev/docs/HARDWARE_W124.md).

## 2. Independent confirmation of callback-driven headless WSI

OrbisDesk's [Mesa WSI patch script](https://github.com/jl-casella/OrbisDesk/blob/dev/tools/vk-h0/patch-mesa-wsi.py) modifies Mesa's headless WSI for PS4:

- The swapchain images are CPU-readable linear buffers; Mesa performs a GPU blit into a host-visible buffer.
- Present waits for the blit fence, then calls a hook with the swapchain, pixel pointer, row pitch, extent, format and wait duration.
- The hook is installed by the Wine ELF and the window layer consumes the latest image for that swapchain.
- The accompanying [wine_vk_runtime.c](https://github.com/jl-casella/OrbisDesk/blob/dev/native/wine_vk_runtime.c) keeps the last presented image per swapchain and notes that the pixels remain valid until the application acquires and presents that image again; the consumer reads it synchronously on the same presenting thread.

This is architecturally close to WinePS4's private-kit 195da48 contract: a headless surface, host-visible linear images, a present-time GPU wait and a frame callback into the desktop. It is strong independent evidence for the design, but not proof that the private kit used identical function names or data structures.

The exact callback and image-lifetime rules are important for a clean implementation: do not retain a raw CPU pixel pointer after the present/acquire lifecycle allows the application to reuse that swapchain image.

References: [patch-mesa-wsi.py](https://github.com/jl-casella/OrbisDesk/blob/dev/tools/vk-h0/patch-mesa-wsi.py), [wine_vk_runtime.c](https://github.com/jl-casella/OrbisDesk/blob/dev/native/wine_vk_runtime.c).

## 3. A separate allocator lesson: libkernel threads and musl's allocator

OrbisDesk's [native_heap.c](https://github.com/jl-casella/OrbisDesk/blob/dev/native/native_heap.c) documents a distinct problem from DolphinPS4's dlmalloc replacement:

- Its linked musl allocator checks an internal libc thread counter before taking bin locks.
- The port creates threads through libkernel, which does not update musl's private thread list.
- OrbisDesk therefore serializes the public allocation family (malloc, calloc, realloc, free, memalign, __memalign, aligned_alloc, posix_memalign) with a recursive lock keyed by scePthreadSelf().
- The lock uses only atomics and sceKernelUsleep; it deliberately avoids allocation, stdio and musl pthread primitives while acquiring the lock.
- The code preserves errno around lock acquisition and delegates to the real musl allocator, rather than modifying private musl thread structures.

This is useful as a warning: if any reconstruction path falls back to the SDK/musl allocator while threads are created directly through libkernel, test its allocator synchronization assumptions. It is not a replacement for WinePS4's direct-first mspace heap, and the wrapper's LGPL-2.1-or-later header does not license unrelated code in the repository.

Reference: [OrbisDesk native_heap.c](https://github.com/jl-casella/OrbisDesk/blob/dev/native/native_heap.c).

## 4. Other relevant runtime findings

OrbisDesk's [HARDWARE_W158.md](https://github.com/jl-casella/OrbisDesk/blob/dev/docs/HARDWARE_W158.md) reports that musl's failed dlopen path stores error state in its own thread structure at FS:0, while the PS4's FS base refers to the system thread block. Calling the stub dlopen/dlsym paths therefore corrupted unrelated thread-block memory. Its fix supplies its own unsupported dynamic-loading functions that do not touch musl TLS.

That is a valuable cross-check for the broader TLS warning already present in the orbis-win research: on Orbis, do not assume the process's FS base is a musl pthread object or that Linux-style dynamic TLS helpers are available. WinePS4's actual executable/static whole-archive configuration still needs its own audit; this is not evidence that it has the same failure.

## 5. What this changes in our reconstruction plan

1. Keep VK_EXT_map_memory_placed and direct-memory aliasing as a required Mesa/Wine integration milestone, not an optional optimization.
2. Add explicit tests for address below 4 GiB, 16 KiB placement alignment, aliasing the same BO pages, and releasing the Wine reservation only after unmap.
3. Specify WSI image lifetime and callback thread semantics in the public kit interface.
4. Keep the shared Wine desktop callback WSI separate from native VideoOut scan-out.
5. If any code path uses musl's allocator with libkernel-created threads, test the musl locking model before assuming normal pthread safety.
6. Do not transplant OrbisDesk's or Crema's implementation wholesale. Use the independent findings to validate the behavior, then implement against the WinePS4 contract and verify file-level licensing.

## 6. Remaining uncertainty

- OrbisDesk's documented W126 fix is a useful hardware-proven analogue, but we have not yet compared its exact Mesa helper against every hunk in WinePS4's private-kit patch series.
- The WinePS4 private heap's arena trigger and dlmalloc revision remain unknown.
- The exact OpenOrbis/musl build used by each project may differ; allocator internals must be checked against the actual selected sysroot.
- A passing host test cannot validate Orbis kernel memory aliasing, 32-bit VA placement or real frame ownership.
