# Public traces collected

This is an evidence log, not a complete reverse engineering of ps4-kit. Links point to public files or immutable commits so the findings can be checked.

## 1. What the public projects say ps4-kit contains

**MGR-PS4 — CREDITS.md**

- Identifies ps4-kit (iHaiDeeZ) as providing toolchain setup, runtime, Mesa RADV for PS4, VideoOut swapchain, crash log, profiler and packaging.
- Says it is linked at build time and "not published yet".

Source: https://github.com/iHaiDeeZ/MGR-PS4/blob/main/CREDITS.md

**MGR-PS4 — docs/DEVELOPMENT.md**

- Documents use of the kit's Mesa RADV and VideoOut swapchain in the Vulkan renderer.
- Mentions kit crash logs (`boot-trace.log`, `crash.log`).
- Documents `ps4-kit/scripts/profile.py --elf` for symbolizing profiler output.

Source: https://github.com/iHaiDeeZ/MGR-PS4/blob/main/docs/DEVELOPMENT.md

**TTT2-PS4 — README.md**

- Says the project needs ps4-kit and records version `ff30ff6` for one build.

Source: https://github.com/iHaiDeeZ/TTT2-PS4/blob/main/README.md

## 2. Direct WinePS4 build interface

From this repository's `app/CMakeLists.txt` and `docs/BUILDING.md`:

| Interface | Observed use | What a replacement must supply |
|---|---|---|
| `$HOME/ps4-kit/ps4-kit.cmake` | Included at CMake configure time | CMake functions, toolchain settings and imported targets |
| `ps4_kit_app(target)` | Final PS4 application target | Link options, entry/runtime setup and executable format |
| `ps4_kit_link_mesa(target)` | Links Mesa/RADV integration | Vulkan loader/driver linkage and required symbols |
| `PS4_LINK_SCRIPT` | Read and modified by WinePS4 to set image base | Compatible linker script |
| `PS4_SYSROOT` | Used for platform headers and libraries | A compatible, legally obtained PS4 sysroot |
| `$PS4_KIT/scripts/make-pkg.sh` | Called by packaging script | Package assembly workflow and required tools |
| `$HOME/ps4-kit/toolchain/env.sh` | Sourced by build script | Compiler/toolchain environment |

These are observed call sites, not a complete specification of the original implementation.

## 3. Historical code clue: stdio locking moved into the kit

Immutable commit:
https://github.com/iHaiDeeZ/TTT2-PS4/commit/cb75f2239ab774d48b885ca64e7b24138ed4a2f6

The commit message says the stdio lock now comes from `ps4-kit (runtime/ps4_stdio.cpp)`. Its diff removes `stdio_lock_ps4.cpp` from TTT2-PS4 and removes linker `--wrap` options for stdio functions. The deleted implementation remains visible in that public diff.

The old implementation is useful as a behavioural specification: it serializes stdio calls with a recursive mutex because the PS4's statically linked musl setup did not initialize per-FILE locks through the usual pthread path. This repository records the finding but does not copy that code; its original licence/redistribution status has not been established. A fresh implementation can be written and tested independently if needed.

## 4. Historical profiler clue

Immutable commit:
https://github.com/iHaiDeeZ/TTT2-PS4/commit/1cac86f2dd8695608697086802f8ada3fe0c7ebc

The commit message says setting `TTT2_PROFILE=1` starts ps4-kit's hot-spot profiler. This confirms a runtime profiling facility exists, but does not reveal its implementation.

## 5. Other public references

- MGR-PS4 credits and development notes: see links above.
- TTT2-PS4: https://github.com/iHaiDeeZ/TTT2-PS4
- DolphinPS4: https://github.com/iHaiDeeZ/DolphinPS4

The public GitHub code-search queries run during this audit did not expose a public `ps4-kit` source repository or a complete copy of its CMake/runtime tree.

## 6. Public runtime primitives used by the reconstruction

The WinePS4 source calls a small set of private-kit runtime helpers. Public PS4 ports expose the underlying system imports:

- Crema's pinned `port/ps4_runtime.cpp` declares and uses `scePthreadSetprio()` and `scePthreadSelf()`, and its thread wrapper raises stack sizes with `pthread_attr_getstacksize()` / `pthread_attr_setstacksize()`.
- PS4SX2's public PS5 compatibility source shows the paired `scePthreadGetprio()` / `scePthreadSetprio()` signatures; PS4 ports also use the same functions in their runtime sources.
- DolphinPS4's public Vulkan probe and several ports use `sceSysmoduleLoadModuleInternal(uint32_t)` to load internal system modules.

Immutable/public references:
- https://github.com/iHaiDeeZ/Crema/blob/475b8d5bd3498dbd5cfefe8175f98d017a017739/port/ps4_runtime.cpp
- https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/probe-vk/main.c
- https://github.com/iHaiDeeZ/PS4SX2/blob/7afa5c43e7827193ac165eecd5b36d34d07761be/ps5/coreorbis/orbis-shims/orbis_eeprof.cpp

The reconstruction now implements WinePS4's thread priority, minimum stack and module-loader calls over these public imports. Its `ps4_kit_heap_options` struct is a new source-level contract inferred from WinePS4's field assignments; it is not claimed to match the unpublished kit's original binary layout. The heap backend that consumes it and the diagnostics backend are still missing.

## 7. Next work items

- [x] Audit the visible `ps4_kit_*` call sites and environment variables in WinePS4; continue the audit as build errors expose additional symbols.
- [ ] Extract the exact compiler/linker command lines and LLVM version from a working kit-era build or reproducible cache.
- [x] Separate known OpenOrbis imports from kit-specific runtime functions where public evidence permits.
- [x] Add the initial `ps4-kit.cmake` compatibility entry point and OpenOrbis linker-script helper.
- [x] Reconstruct the initial six-argument package builder and add fake-tool tests.
- [ ] Reconstruct the heap backend and diagnostics runtime; the heap-options source contract is provisional.
- [ ] Reconstruct the Wine-specific RADV/WSI path and complete static link closure.
- [ ] Run all host tests through CI, then validate against the user's real OpenOrbis tools and sysroot.
- [ ] Verify the final package and runtime on hardware; do not claim buildability from static inspection alone.
