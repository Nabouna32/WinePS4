# Mesa / RADV reconstruction notes

This directory is reserved for the Mesa GNM build and Wine window-mode WSI.
No Mesa source tree or compiled archive is vendored here.

## Candidate public baseline

A recent public PS4 port documents a reproducible Mesa build environment:

- Mesa 26.0.8 in `$PS4_BUILD_ROOT/src/mesa`;
- target sysroot in `$PS4_BUILD_ROOT/sysroot`;
- Meson build in `$PS4_BUILD_ROOT/build/mesa`;
- host Meson/Mako/PyYAML environment and `glslangValidator` under the build root;
- a PS4 GNM implementation of Mesa's AMDGPU kernel interface, `src/amd/common/ac_ps4_drm.c`.

The GNM winsys file is explicitly marked MIT. The public build recipe and archive lists are recorded in `docs/ps4-kit-reconstruction/MESA-LINK-TRACE.md`.

The environment entry point now exports:

- `PS4_BUILD_ROOT` (defaults to `PSB`);
- `PS4_MESA_SOURCE` (defaults to `$PS4_BUILD_ROOT/src/mesa`);
- `PS4_MESA_BUILD` (defaults to `$PS4_BUILD_ROOT/build/mesa`).

This is a candidate build layout from a later public port, not proof of the exact Mesa commit or build options used by WinePS4's private ps4-kit revision `56b3d86`.

The candidate whole-archive closure from the public DolphinPS4 CMake helper is recorded in `archives.cmake`. Its resolver checks every expected archive under `PS4_MESA_BUILD`; the host CMake tests verify both the complete closure and the missing-archive diagnostic. It is now wired into `ps4_kit_link_mesa()`, which validates the archives and adds them with whole-archive semantics. The function still stops until the matching Mesa source headers and Wine-specific WSI source are available.

## Why there is no build script yet

The public build script lists all Meson archive targets it builds, while the matching CMake helper links the final whole-archive closure. Its comments explicitly say `libvulkan_radeon.a` already contains the Vulkan runtime and WSI objects, explaining why the helper does not link the standalone runtime/WSI archives a second time. The closure is therefore usable as a candidate when paired with the same Mesa revision.

WinePS4 needs a window-sized, CPU-readable headless swapchain that calls `ps4kit_window_present()`; a full-screen VideoOut WSI is not compatible. The first clean-room implementation is now in `ps4_vk_wsi_window.cpp`. It creates linear host-visible/coherent images, uses the actual subresource row pitch, consumes presentation wait semaphores with a queue submit + fence, and calls the desktop callback only after GPU completion. It intentionally does not open VideoOut or use the public full-screen flip path.

The callback ABI and lifecycle requirements are recorded in [WINDOW-WSI-CONTRACT.md](../../docs/ps4-kit-reconstruction/WINDOW-WSI-CONTRACT.md). Exact source files, commit IDs, license provenance, and the distinction between technical references and code copied are recorded in [VULKAN-WSI-SOURCES.md](../../docs/ps4-kit-reconstruction/VULKAN-WSI-SOURCES.md).

The dispatch path is no longer an open question: unchanged `port/ps4_unixlibs.c` already forwards the synthetic `libvulkan.so.1` entry points to `ps4kit_vk_get_instance_proc_addr()`, which the new WSI exports. The next blockers are target-specific: pin the Mesa/Vulkan-Headers revision, confirm RADV supports the advertised linear image formats/usages and coherent memory types, verify the queue/fence behavior, then exercise resize/recreation and the desktop callback on a PS4. The deliberate `ps4_kit_link_mesa()` guard stays until those checks pass.
