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

The candidate whole-archive closure from the public DolphinPS4 CMake helper is recorded in `archives.cmake`. Its resolver checks every expected archive under `PS4_MESA_BUILD`; it is deliberately not wired into `ps4_kit_link_mesa()` until the matching Mesa build and Wine-specific WSI are available.

## Why there is no build script yet

The public build script lists all Meson archive targets it builds, while the matching CMake helper links the final whole-archive closure. Its comments explicitly say `libvulkan_radeon.a` already contains the Vulkan runtime and WSI objects, explaining why the helper does not link the standalone runtime/WSI archives a second time. The closure is therefore usable as a candidate when paired with the same Mesa revision.

WinePS4 still needs a window-sized, CPU-readable headless swapchain that calls `ps4kit_window_present()`; a full-screen VideoOut WSI is not compatible. Before replacing the deliberate `ps4_kit_link_mesa()` failure, we must pin the Mesa source/patch set and implement the Wine-specific WSI against that exact revision.
