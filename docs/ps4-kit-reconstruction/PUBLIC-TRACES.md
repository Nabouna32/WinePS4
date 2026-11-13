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

## 6. Next work items

- [ ] Audit every `ps4_kit_*` call and every `PS4_*` variable in WinePS4.
- [ ] Extract the exact compiler/linker command lines from the build scripts and CMake files.
- [ ] Identify OpenOrbis-provided pieces versus kit-specific pieces.
- [ ] Design a minimal `ps4-kit.cmake` compatibility layer.
- [ ] Document the required linker script and executable format.
- [ ] Map Mesa/RADV and VideoOut dependencies before attempting a graphics replacement.
- [ ] Recreate package assembly only after the executable build is understood.
- [ ] Verify each stage on hardware; do not claim buildability from static inspection alone.
