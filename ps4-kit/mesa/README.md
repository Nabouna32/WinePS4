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

The GNM winsys file is explicitly marked MIT. DolphinPS4's `CREDITS.md` describes the Mesa port as MIT/per-file, and this tree carries separate notices for the winsys and integration patch. There is a provenance discrepancy to keep visible: OrbisDesk's `THIRD_PARTY.md` describes the Mesa patches originating in DolphinPS4 as GPL-2.0-or-later while identifying the winsys itself as MIT. Preserve the per-file notices and re-check exact provenance before copying further hunks; do not infer a blanket licence from the repository root. `prepare-source.sh` uses `git apply --check` before applying the patch to an external Mesa checkout, then stages the winsys. It fails closed if the patch does not match.

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

## Real upstream patch check

GitHub Actions run [#265](https://github.com/Nabouna32/WinePS4/actions/runs/43503275708) now fetches the official upstream `mesa-26.0.8` tag from Mesa's GitLab repository and runs `prepare-source.sh` against that real source tree. The dedicated `mesa-upstream-patch` job passed: the patch applies cleanly, the GNM source is staged, and `git diff --check` reports no whitespace errors. This validates patch applicability to the public 26.0.8 tag; it does not yet validate the PS4 cross-build or resulting archives.


## Wine-specific placed mappings (not implemented yet)

OrbisDesk's W126 hardware test confirms that 32-bit DXVK needs a CPU-only alias of the same direct-memory BO pages at Wine's reserved address, using `sceKernelMapDirectMemory(..., MAP_FIXED, ...)` and 16 KiB alignment. Its generator is based on Crema's Wine winsys, not the DolphinPS4 file currently staged here: the generator expects a `write_combined` flag and `ps4_flush_cpu_cache()` cache-coherency helper absent from our current source. The cache flush is required to prevent dirty cached lines from a previous onion owner overwriting data after pages are reused as write-combined garlic.

Do not apply `patch-mesa-placed.py` verbatim. First port the missing coherency prerequisites (or deliberately select and audit Crema's complete winsys), then implement fixed mapping and inspect RADV's unmap path and Wine's reservation ownership. Also resolve the private-kit `sceKernelEnableDmemAliasing` prerequisite. See [the independent Wine-port evidence](../../docs/ps4-kit-reconstruction/INDEPENDENT-WINE-PORT-EVIDENCE.md) and [the Mesa integration trace](../../docs/ps4-kit-reconstruction/MESA-LINK-TRACE.md).
