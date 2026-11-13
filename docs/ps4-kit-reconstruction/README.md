# ps4-kit — reconstruction notes

This directory tracks a **clean-room compatibility effort** for the build interface used by WinePS4.

It is not the original ps4-kit, and it does not contain Sony SDK files, proprietary libraries, leaked binaries, or code copied from repositories without a clear redistribution licence. The goal is to document public interfaces and, later, implement independent replacements where legally and technically possible.

## Why this exists

The WinePS4 build currently depends on an external, unpublished kit:

- `app/CMakeLists.txt` includes `$HOME/ps4-kit/ps4-kit.cmake`.
- It calls `ps4_kit_app(wineps4)` and `ps4_kit_link_mesa(wineps4)`.
- It uses kit-provided build configuration such as `PS4_LINK_SCRIPT` and `PS4_SYSROOT`.
- `docs/BUILDING.md` says the kit is not published and records the version used for the v00.71 build (`56b3d86`).

A public repository by the same author, MGR-PS4, describes ps4-kit as providing toolchain setup, runtime, PS4 Mesa RADV, a VideoOut swapchain, crash logging, a profiler, and packaging. It also says the kit is linked at build time and is not published yet.

## Current strategy

Rebuild the **minimum compatible interface**, not an imagined byte-for-byte copy of the private kit. Keep the WinePS4 application sources and their behavior unchanged: all compatibility work belongs under the reconstructed `ps4-kit/` tree (plus research documentation/tests). Install it at `$HOME/ps4-kit` with a symlink so the existing build scripts and CMake includes continue to work.

1. Inventory every CMake function, variable, header, library and script WinePS4 expects.
2. Separate build-host tools from PS4 runtime components.
3. Identify which components can use OpenOrbis and public upstream projects.
4. Write independent CMake/toolchain glue and small compatibility implementations.
5. Keep unavailable pieces explicitly marked as blockers until there is a lawful, testable replacement.
6. Build and test incrementally on a PS4 development environment.

## Status

**Build-first scope:** [BUILD-RECONSTRUCTION-PLAN.md](BUILD-RECONSTRUCTION-PLAN.md) is the active plan and acceptance checklist. See [PUBLIC-TRACES.md](PUBLIC-TRACES.md) for public evidence, [INTERFACE-AUDIT.md](INTERFACE-AUDIT.md) for the build/runtime boundary, and [MESA-LINK-TRACE.md](MESA-LINK-TRACE.md) for the Mesa GNM/RADV and Wine window-WSI build path. [NETWORK-API-MAP.md](NETWORK-API-MAP.md) is background only; networking is not a priority.

The repository now contains the kit entry points, runtime/package prototypes, a first Wine window-mode WSI, the MIT/per-file Mesa 26.0.8 GNM winsys and integration patch, and a Mesa cross-build driver. Host tests cover the scripts, CMake/link glue, runtime wrappers and WSI dispatch. See [`ps4-kit/README.md`](../../ps4-kit/README.md) for the no-Wine-source-edit path. This is not yet a complete or target-validated kit: the exact private Mesa/LLVM revisions, actual OpenOrbis target build, final linker closure, eboot/package output and PS4 presentation still need validation.

## Rules for contributions

- Prefer original implementations and documented public APIs.
- Record the source URL, commit, file path and licence for every reused component.
- Do not commit Sony SDK files, private keys, console credentials, or proprietary binaries.
- Do not describe this directory as the original ps4-kit.
- Keep experimental code separate from WinePS4's production build until it passes a real build.


## Dependency recovery

The reconstruction now follows an explicit reuse-first inventory: [ECOSYSTEM-ANCESTRY.md](ECOSYSTEM-ANCESTRY.md) and [HEAP-RUNTIME-TRACE.md](HEAP-RUNTIME-TRACE.md) and [WINE-PORT-COMPARISON.md](WINE-PORT-COMPARISON.md), which compares an independent Wine-on-PS4 port (orbis-win) without conflating its native VideoOut WSI with WinePS4's desktop callback WSI., which traces the Wine-specific heap options, process-role budgets, private kit commit prefixes (`195da48`, `a306bf5`, `bb4a45c`, `941b771`, `53cf427`), thread priorities and the public love-ps4 → dlmalloc runtime lineage. broadens provenance research beyond the original author, records the explicit WinePS4 credits trail (Resonance, DolphinPS4, M33, Crema), and compares love-ps4, OpenOrbis/PacBrew, orbis-ports, PS4MC, orbisdev and the older PS4SDK lineage.  [DEPENDENCY-RECOVERY.md](DEPENDENCY-RECOVERY.md) traces PacBrew's PS4 toolchain/portlibs, the DolphinPS4 → Crema/Resonance → M33 build-tree chain, WinePS4's exact FreeType 2.13.3 and FFmpeg 7.1.1 requirements, and the separate PS4MC/Kodi dependency recipes. [ARTIFACT-RECOVERY.md](ARTIFACT-RECOVERY.md) records the publicly downloadable SMW PS4 SDK (sysroot + static Mesa archives), its SHA-256 and contents-generation script, the separate PacBrew bootstrap route, and the RPCS3 → M33 FFmpeg artifact lineage. It distinguishes proven downloadable artifacts from candidate inputs that still require ABI/build validation.
