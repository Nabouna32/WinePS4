# Mesa / RADV reconstruction notes

This directory is reserved for the Mesa GNM build and Wine window-mode WSI.
No Mesa source tree or compiled archive is vendored here. The PS4 GNM winsys source `ac_ps4_drm.c` is included with its upstream MIT SPDX notice.

## Candidate public baseline

A recent public PS4 port documents a reproducible Mesa build environment:

- Mesa 26.0.8 in `$PS4_BUILD_ROOT/src/mesa`;
- target sysroot in `$PS4_BUILD_ROOT/sysroot`;
- Meson build in `$PS4_BUILD_ROOT/build/mesa`;
- host Meson/Mako/PyYAML environment and `glslangValidator` under the build root;
- a PS4 GNM implementation of Mesa's AMDGPU kernel interface, `src/amd/common/ac_ps4_drm.c`.

The GNM winsys file is explicitly marked MIT. The matching public PS4 Mesa integration patch is also identified as MIT/per-file in DolphinPS4's `CREDITS.md`; both are included with separate MIT notices. `prepare-source.sh` uses `git apply --check` before applying the patch to an external Mesa checkout, then stages the winsys. It fails closed if the patch does not match.

The environment entry point now exports:

- `PS4_BUILD_ROOT` (defaults to `PSB`);
- `PS4_MESA_SOURCE` (defaults to `$PS4_BUILD_ROOT/src/mesa`);
- `PS4_MESA_BUILD` (defaults to `$PS4_BUILD_ROOT/build/mesa`).

This is a candidate build layout from a later public port, not proof of the exact Mesa commit or build options used by WinePS4's private ps4-kit revision `56b3d86`.

The candidate whole-archive closure from the public DolphinPS4 CMake helper is recorded in `archives.cmake`. Its resolver checks every expected archive under `PS4_MESA_BUILD`; the host CMake tests verify both the complete closure and the missing-archive diagnostic. It is now wired into `ps4_kit_link_mesa()`, which validates the archives and adds them with whole-archive semantics. The function still stops until the matching Mesa source tree, Vulkan-Headers sysroot, and all candidate archives are available.

## Building the candidate Mesa baseline

`ps4-kit/scripts/build-mesa.sh` is a clean-room build driver based on the public DolphinPS4 Mesa 26.0.8 recipe. It requires the OpenOrbis target sysroot, host Meson/Mako/PyYAML environment, host `glslangValidator`, and header-only libdrm source tree. It applies the bundled Mesa patch only when it matches the Git checkout, stages the GNM winsys, creates the PS4 Meson cross file, and builds the 19 public static RADV targets. The final WinePS4 CMake link closure is the 16-archive subset in `archives.cmake`; the separate Vulkan runtime/WSI archives are built as in the public recipe but are not linked twice when already folded into `libvulkan_radeon.a`.

WinePS4 needs a window-sized, CPU-readable headless swapchain that calls `ps4kit_window_present()`; a full-screen VideoOut WSI is not compatible. The first clean-room implementation is now in `ps4_vk_wsi_window.cpp`. It creates linear host-visible/coherent images, uses the actual subresource row pitch, consumes presentation wait semaphores with a queue submit + fence, and calls the desktop callback only after GPU completion. It intentionally does not open VideoOut or use the public full-screen flip path.

The callback ABI and lifecycle requirements are recorded in [WINDOW-WSI-CONTRACT.md](../../docs/ps4-kit-reconstruction/WINDOW-WSI-CONTRACT.md). Exact source files, commit IDs, license provenance, and the distinction between technical references and code copied are recorded in [VULKAN-WSI-SOURCES.md](../../docs/ps4-kit-reconstruction/VULKAN-WSI-SOURCES.md).

The dispatch path is no longer an open question: unchanged `port/ps4_unixlibs.c` already forwards the synthetic `libvulkan.so.1` entry points to `ps4kit_vk_get_instance_proc_addr()`, which the new WSI exports. The next blockers are target-specific: pin the Mesa/Vulkan-Headers revision, confirm RADV supports the advertised linear image formats/usages and coherent memory types, verify the queue/fence behavior, then exercise resize/recreation and the desktop callback on a PS4. The deliberate `ps4_kit_link_mesa()` guard stays until those checks pass.
