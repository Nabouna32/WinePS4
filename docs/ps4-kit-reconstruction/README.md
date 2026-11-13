# ps4-kit — reconstruction notes

This directory tracks a **clean-room compatibility effort** for the shared PS4
build/runtime base used by the developer's public ports. WinePS4 remains the
first unchanged-source build target; the longer-term goal is a versioned kit
that can also support MGR-PS4 and the shared toolchain/runtime lineage behind
DolphinPS4, Crema, Resonance, M33, SMW and TOTK-PS4.

It is not the original private ps4-kit. It contains no Sony SDK files,
proprietary libraries, leaked binaries, game data or private keys. The goal is
to document public interfaces and implement independent replacements where
lawful and technically testable.

## Why this exists

WinePS4's public build depends on an unpublished external kit:

- `app/CMakeLists.txt` includes `$HOME/ps4-kit/ps4-kit.cmake`.
- It calls `ps4_kit_app(wineps4)` and `ps4_kit_link_mesa(wineps4)`.
- It uses kit-provided build configuration such as `PS4_LINK_SCRIPT` and
  `PS4_SYSROOT`.
- `docs/BUILDING.md` records private kit revision `56b3d86`.

MGR-PS4 independently documents private kit revision `79fba0f`, sources the
same `toolchain/env.sh`, injects `ps4-kit.cmake` into its CMake project, and
credits the kit for runtime, RADV, VideoOut, diagnostics and packaging. These
two revision identifiers prove that the compatibility target is broader than
WinePS4 alone.

## Current strategy

Rebuild a **versioned public compatibility layer**, not an imagined byte-for-byte
copy of the private kit. Keep WinePS4 application sources and behavior unchanged:
all missing compatibility behavior belongs at the kit boundary. The intended
install path remains `$HOME/ps4-kit`.

1. Inventory each project's CMake functions, variables, headers, libraries,
   scripts and required runtime behavior.
2. Separate host tools from the PS4 target toolchain and sysroot.
3. Reuse public OpenOrbis/PacBrew packages and public build artifacts when
   versions, ABI and licences match.
4. Separate shared runtime/RADV code from project-specific presentation
   backends and heap policies.
5. Implement explicit project adapters and lock down source/toolchain versions.
6. Keep unavailable pieces marked as blockers until a lawful, testable
   replacement exists.

## Status

[BUILD-RECONSTRUCTION-PLAN.md](BUILD-RECONSTRUCTION-PLAN.md) remains the active
Wine-first plan. [PROJECT-COMPATIBILITY-MATRIX.md](PROJECT-COMPATIBILITY-MATRIX.md)
now records direct kit consumers, related projects, evidence, and the profiles
needed for broader compatibility. [PUBLIC-TRACES.md](PUBLIC-TRACES.md) records
public evidence; [INTERFACE-AUDIT.md](INTERFACE-AUDIT.md) covers the
build/runtime boundary; [MESA-LINK-TRACE.md](MESA-LINK-TRACE.md) follows the
Mesa GNM/RADV and Wine window-WSI build path.

The repository contains kit entry points, runtime/package prototypes, a first
Wine window-mode WSI, the MIT/per-file Mesa 26.0.8 GNM winsys and integration
patch, and a Mesa cross-build driver. Host tests cover scripts, CMake/link glue,
runtime wrappers and WSI dispatch.

This is **not yet a complete or target-validated kit**. The exact private Mesa
and LLVM revisions, actual OpenOrbis target build, final linker closure, eboot
and package output, MGR's native VideoOut path, and PS4 presentation still need
validation.

## Research map

- [ECOSYSTEM-ANCESTRY.md](ECOSYSTEM-ANCESTRY.md) traces WinePS4's explicit
  credits to Resonance, DolphinPS4, M33 and Crema, alongside love-ps4,
  OpenOrbis/PacBrew, orbis-ports, PS4MC, orbisdev and historical PS4SDK sources.
- [HEAP-RUNTIME-TRACE.md](HEAP-RUNTIME-TRACE.md) records Wine's heap options,
  process-role budgets, private kit commit prefixes, thread priorities and
  public love-ps4 → dlmalloc lineage.
- [ALLOCATOR-ENGINE-COMPARISON.md](ALLOCATOR-ENGINE-COMPARISON.md) compares
  DolphinPS4's public dlmalloc/mspace implementation with Wine's different
  direct-first, overflow and automatic-arena contract.
- [INDEPENDENT-WINE-PORT-EVIDENCE.md](INDEPENDENT-WINE-PORT-EVIDENCE.md)
  records OrbisDesk's physical confirmation of placed memory below 4 GiB for
  32-bit DXVK and the cache-coherency prerequisites.
- [WINE-PORT-COMPARISON.md](WINE-PORT-COMPARISON.md) compares the independent
  orbis-win Wine port without conflating its native VideoOut WSI with
  WinePS4's desktop callback WSI.
- [DEPENDENCY-RECOVERY.md](DEPENDENCY-RECOVERY.md) traces PacBrew's PS4
  toolchain/portlibs, DolphinPS4 → Crema/Resonance → M33 build-tree lineage,
  WinePS4's exact FreeType 2.13.3 and FFmpeg 7.1.1 requirements, and PS4MC
  dependency recipes.
- [ARTIFACT-RECOVERY.md](ARTIFACT-RECOVERY.md) records the public SMW PS4 SDK
  artifact, its SHA-256 and generation script, the PacBrew bootstrap route,
  and the RPCS3 → M33 FFmpeg artifact lineage.
- [NETWORK-API-MAP.md](NETWORK-API-MAP.md) is background only; networking is
  not a current priority.
- [VULKAN-WSI-SOURCES.md](VULKAN-WSI-SOURCES.md) and
  [WINDOW-WSI-CONTRACT.md](WINDOW-WSI-CONTRACT.md) cover distinct Vulkan
  presentation paths.

## Contribution rules

- Prefer original implementations and documented public APIs.
- Record source URL, commit, file path and licence for every reused component.
- Do not commit Sony SDK files, private keys, console credentials or
  proprietary binaries.
- Do not describe this directory as the original ps4-kit.
- Keep experimental code separate from WinePS4's production build until it
  passes a real build.
