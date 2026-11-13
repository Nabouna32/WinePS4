# Direct ps4-kit consumer audit

Status: source/build-script audit, not an end-to-end build. The private kit is
unpublished, so a public consumer's version string or build invocation is
evidence of a compatibility requirement, not a copy of the private source.

## 1. Confirmed direct consumers

| Consumer | Public kit revision | Build entry point | Confirmed kit contract | Profile |
|---|---:|---|---|---|
| [WinePS4](https://github.com/iHaiDeeZ/WinePS4) | `56b3d86` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake`; includes `ps4-kit.cmake` in app and launcher | `ps4_kit_app()`, `ps4_kit_link_mesa()`, linker script, target sysroot, runtime, package script | `window-desktop` |
| [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4) | `79fba0f` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake`; `CMAKE_PROJECT_sa2_INCLUDE=...`; `PS4_KIT_NETFS=OFF` | project() injection, shared PS4 linker/runtime/Mesa/VideoOut base, `make-pkg.sh`; builds `sa2_eboot` | `native-videoout`, ReXGlue |
| [MP3-PS4](https://github.com/iHaiDeeZ/MP3-PS4) | not stated in checked public build docs | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake`; `CMAKE_PROJECT_mp3_INCLUDE=...`; `PS4_KIT_NETFS=OFF` | same project() injection pattern, shared ReXGlue runtime/RADV/VideoOut base and `make-pkg.sh`; builds `mp3_eboot` | `native-videoout`, ReXGlue |
| [TTT2-PS4](https://github.com/iHaiDeeZ/TTT2-PS4) | `ff30ff6` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake` on `port/ps4`; kit package script | shared runtime, Mesa RADV, VideoOut swapchain, packaging; builds `ttt2_eboot` | `native-videoout`, XenonRecomp/DXVK |

The MGR and MP3 scripts are unusually valuable because they expose the exact
CMake hook names and the same explicit optional-component setting:

- MGR: `CMAKE_PROJECT_sa2_INCLUDE="$PS4_KIT/ps4-kit.cmake"`
- MP3: `CMAKE_PROJECT_mp3_INCLUDE="$PS4_KIT/ps4-kit.cmake"`
- Both pass `-DPS4_KIT_NETFS=OFF`.

This is a real shared interface: CMake includes the kit file immediately after
the matching `project(sa2 ...)` or `project(mp3 ...)` call. The kit must be
safe at that point in configuration, before the rest of the project's
CMakeLists has declared its executable target.

### What is and is not established

- MGR's public script asks Ninja to build `sa2_eboot`; MP3 asks for
  `mp3_eboot`. Their public build scripts do not call Wine's
  `ps4_kit_app()` or `ps4_kit_link_mesa()` directly. Do not assume that the
  Wine target helpers are the entire private-kit contract for ReXGlue ports.
- Their credits identify the kit as supplying runtime, RADV, VideoOut,
  diagnostics/profiling and packaging. The public project patches also contain
  their own PS4 platform code. Exact division of ownership between project and
  kit still needs a symbol/link-map audit.
- `PS4_KIT_NETFS=OFF` is confirmed as a consumer setting. Its private
  implementation semantics are not public. The current clean-room CMake file
  does not implement a NetFS component; accepting the cache variable is not
  proof of NetFS compatibility.
- TTT2's public docs name kit revision `ff30ff6`. Its CMake build is driven
  from `port/ps4`, while the script uses the kit environment and package
  helper. Its exact CMake-to-kit coupling needs a separate source audit.
- No source or binary from the private kit has been recovered. Do not recreate
  private commit history or claim byte-for-byte compatibility.

## 2. Related ports that reuse the same graphics/runtime lineage

| Project | Relationship | What it can validate |
|---|---|---|
| [SSBU](https://github.com/iHaiDeeZ/SSBU) | Its build docs explicitly seed SMW's libc++ sysroot and Crema's Mesa; its VideoOut WSI says it descends from SMW/Resonance/Crema | native VideoOut WSI behavior, DXVK/RADV integration, runtime and package contracts; not proof of current private-kit consumption |
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | Public Clang 21/libc++ 21, GNM winsys and RADV baseline | toolchain, Mesa source/build and GNM primitive tests |
| [Crema](https://github.com/iHaiDeeZ/Crema) | Later GNM winsys with write-combined/cache-flush support | Mesa lifecycle and CPU/GPU cache coherency |
| [Resonance](https://github.com/iHaiDeeZ/Resonance) | Earlier VideoOut WSI and common runtime/build-tree ancestry | native VideoOut semantics and shared runtime provenance |
| [M33](https://github.com/iHaiDeeZ/M33), [SMW](https://github.com/iHaiDeeZ/SMW), [xemu-ps4](https://github.com/iHaiDeeZ/xemu-ps4), [TOTK-PS4](https://github.com/iHaiDeeZ/TOTK-PS4) | Consume or adapt the public Resonance/Crema/Dolphin/SMW build lineage | reproducible sysroot/Mesa bundle and native WSI integration |

WinePS4's notes explicitly say TTT2 and SSBU drew on the private kit's RADV
and VideoOut WSI, while Wine itself required a separate window-desktop mode.
That is why the native WSI must be a separate kit profile rather than being
folded into the Wine implementation.

## 3. Host configure regression test added

Run:

```bash
bash ps4-kit/tests/test-project-injection.sh
```

The test creates a temporary OpenOrbis-shaped fixture (a minimal linker script
and placeholder `create-fself`) and configures two tiny CMake projects:

- `project(sa2 C CXX)`, with `CMAKE_PROJECT_sa2_INCLUDE`;
- `project(mp3 C CXX)`, with `CMAKE_PROJECT_mp3_INCLUDE`.

It checks that the kit hook runs at the correct CMake phase, the runtime target
and the public helper functions exist, the build-local linker script is
generated, and `PS4_KIT_NETFS=OFF` survives configuration. It is a host-only
configure test: it does not compile the PS4 runtime, link Mesa, create an eboot
or prove that the private NetFS behavior has been reproduced.

## 4. Next implementation gates

1. Run the new injection test and add it to the repository CI.
2. Audit the full MGR and MP3 patched CMakeLists and generated link commands;
   identify which target receives runtime, RADV archives, ICD entry point,
   VideoOut WSI and system-library imports.
3. Add a generic ReXGlue/native-videoout kit profile only after that audit.
   Keep `PS4_KIT_NETFS=OFF` explicit and avoid implementing or advertising
   NetFS until its required symbols and semantics are known.
4. Audit TTT2's `port/ps4/CMakeLists.txt` and runtime target separately; it
   has a different integration path from MGR/MP3.
5. Reuse the public Resonance/Crema-derived WSI only after recording exact
   source provenance and per-file licence. The repository-level licence alone
   is not sufficient evidence for copying an individual file.
6. Keep Wine's `window-desktop` callback, placed-memory mapping and
   direct-first heap policy isolated from the native-videoout profiles.

The acceptance criterion is not merely "CMake configured": each direct
consumer must eventually configure, compile, link its actual target, produce
its own package, and have runtime behavior tested independently.
