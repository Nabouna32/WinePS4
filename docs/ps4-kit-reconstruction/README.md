# ps4-kit — reconstruction notes

This directory tracks a **clean-room compatibility effort** for the build interface used by WinePS4.

It is not the original ps4-kit, and it does not contain Sony SDK files, proprietary libraries, leaked binaries, or code copied from repositories without a clear redistribution licence. The goal is to document public interfaces and implement independent replacements where lawful and technically testable.

## Why this exists

The WinePS4 build depends on an external, unpublished kit:

- `app/CMakeLists.txt` includes `$HOME/ps4-kit/ps4-kit.cmake`.
- It calls `ps4_kit_app(wineps4)` and `ps4_kit_link_mesa(wineps4)`.
- It uses kit-provided build configuration such as `PS4_LINK_SCRIPT` and `PS4_SYSROOT`.
- `docs/BUILDING.md` records the version used for the v00.71 build (`56b3d86`).

The public MGR-PS4 repository by the same author describes ps4-kit as providing toolchain setup, runtime, PS4 Mesa RADV, VideoOut swapchain, crash logging, profiler and packaging. It also says the kit is linked at build time and is not published.

## Current strategy

Rebuild the **minimum compatible interface**, not an imagined byte-for-byte copy of the private kit. Keep WinePS4 application sources and behavior unchanged: compatibility work belongs under the reconstructed `ps4-kit/` tree, with research documentation and tests alongside it. The intended install path is `$HOME/ps4-kit`, matching existing build scripts and CMake includes.

1. Inventory every CMake function, variable, header, library and script WinePS4 expects.
2. Separate build-host tools from PS4 runtime components.
3. Identify components that can use OpenOrbis and public upstream projects.
4. Write independent CMake/toolchain glue and small compatibility implementations.
5. Keep unavailable pieces explicitly marked as blockers until a lawful, testable replacement exists.
6. Build and test incrementally on a PS4 development environment.

## Status

[BUILD-RECONSTRUCTION-PLAN.md](BUILD-RECONSTRUCTION-PLAN.md) is the active plan and acceptance checklist. [PUBLIC-TRACES.md](PUBLIC-TRACES.md) records public evidence; [INTERFACE-AUDIT.md](INTERFACE-AUDIT.md) covers the build/runtime boundary; [MESA-LINK-TRACE.md](MESA-LINK-TRACE.md) follows the Mesa GNM/RADV and Wine window-WSI build path.

The repository contains kit entry points, runtime/package prototypes, a first Wine window-mode WSI, the MIT/per-file Mesa 26.0.8 GNM winsys and integration patch, and a Mesa cross-build driver. Host tests cover scripts, CMake/link glue, runtime wrappers and WSI dispatch. See [`ps4-kit/README.md`](../../ps4-kit/README.md) for the no-Wine-source-edit path.

This is **not yet a complete or target-validated kit**. The exact private Mesa/LLVM revisions, actual OpenOrbis target build, final linker closure, eboot/package output and PS4 presentation still need validation.

## Research map

- [ECOSYSTEM-ANCESTRY.md](ECOSYSTEM-ANCESTRY.md) traces WinePS4's explicit credits to Resonance, DolphinPS4, M33 and Crema, alongside love-ps4, OpenOrbis/PacBrew, orbis-ports, PS4MC, orbisdev and historical PS4SDK sources.
- [HEAP-RUNTIME-TRACE.md](HEAP-RUNTIME-TRACE.md) records the Wine-specific heap options, process-role budgets, private kit commit prefixes, thread priorities and public love-ps4 → dlmalloc lineage.
- [ALLOCATOR-ENGINE-COMPARISON.md](ALLOCATOR-ENGINE-COMPARISON.md) compares DolphinPS4's public dlmalloc/mspace implementation with Wine's different direct-first, overflow and automatic-arena contract. It also maps the public OpenOrbis memory APIs and licensing boundary.
- [WINE-PORT-COMPARISON.md](WINE-PORT-COMPARISON.md) compares the independent orbis-win Wine port without conflating its native VideoOut WSI with WinePS4's desktop callback WSI.
- [DEPENDENCY-RECOVERY.md](DEPENDENCY-RECOVERY.md) traces PacBrew's PS4 toolchain/portlibs, DolphinPS4 → Crema/Resonance → M33 build-tree lineage, WinePS4's exact FreeType 2.13.3 and FFmpeg 7.1.1 requirements, and PS4MC/Kodi dependency recipes.
- [ARTIFACT-RECOVERY.md](ARTIFACT-RECOVERY.md) records the public SMW PS4 SDK artifact (sysroot and static Mesa archives), its SHA-256 and generation script, the separate PacBrew bootstrap route, and the RPCS3 → M33 FFmpeg artifact lineage.
- [NETWORK-API-MAP.md](NETWORK-API-MAP.md) is background only; networking is not a current priority.
- [VULKAN-WSI-SOURCES.md](VULKAN-WSI-SOURCES.md) and [WINDOW-WSI-CONTRACT.md](WINDOW-WSI-CONTRACT.md) cover the distinct Vulkan/window presentation paths.

## Contribution rules

- Prefer original implementations and documented public APIs.
- Record source URL, commit, file path and licence for every reused component.
- Do not commit Sony SDK files, private keys, console credentials or proprietary binaries.
- Do not describe this directory as the original ps4-kit.
- Keep experimental code separate from WinePS4's production build until it passes a real build.
