# Direct ps4-kit consumer audit

Status: source/build-script audit, not an end-to-end build. The private kit is
unpublished, so a public consumer's version string or build invocation is
evidence of a compatibility requirement, not a copy of the private source.

## 1. Confirmed direct consumers

| Consumer | Public kit revision | Build entry point | Confirmed kit contract | Profile |
|---|---:|---|---|---|
| [WinePS4](https://github.com/iHaiDeeZ/WinePS4) | `56b3d86` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake`; includes `ps4-kit.cmake` in app and launcher | `ps4_kit_app()`, `ps4_kit_link_mesa()`, linker script, target sysroot, runtime, package script | `window-desktop` |
| [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4) | `79fba0f` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake`; `CMAKE_PROJECT_sa2_INCLUDE=...`; `PS4_KIT_NETFS=OFF` | project() injection, shared PS4 linker/runtime/Mesa/VideoOut base, `make-pkg.sh`; builds `sa2_eboot` | `native-videoout`, ReXGlue |
| [MP3-PS4](https://github.com/iHaiDeeZ/MP3-PS4) | `56b3d86` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake`; `CMAKE_PROJECT_mp3_INCLUDE=...`; `PS4_KIT_NETFS=OFF` | same project() injection pattern, shared ReXGlue runtime/RADV/VideoOut base and `make-pkg.sh`; builds `mp3_eboot` | `native-videoout`, ReXGlue |
| [TTT2-PS4](https://github.com/iHaiDeeZ/TTT2-PS4) | `ff30ff6` | `source ~/ps4-kit/toolchain/env.sh`; `ps4_cmake` on `port/ps4`; kit package script | shared runtime, Mesa RADV, VideoOut swapchain, packaging; builds `ttt2_eboot` | `native-videoout`, XenonRecomp/DXVK |
| [ConkerLR-PS4](https://github.com/iHaiDeeZ/ConkerLR-PS4) | `87a517c` | `scripts/build-clx.sh` sources `~/ps4-kit/toolchain/env.sh`; package step calls `$PS4_KIT/scripts/make-pkg.sh` | shared runtime, Mesa RADV, VideoOut swapchain and packaging; `libs` mode builds runtime targets without an XBE | `native-videoout`, Xbox recomp |
| [Godot PS4 branch](https://github.com/iHaiDeeZ/godot/tree/ps4/platform/ps4) | not pinned in public README | `platform/ps4/build.sh`; eboot CMake includes `$PS4_KIT/ps4-kit.cmake` | calls `ps4_kit_link_mesa()` and `ps4_kit_app()`; uses kit runtime, linker wraps, Mesa and VideoOut presenter | `native-videoout`, SCons engine build |

MP3's current README explicitly says release v00.51 was built with kit revision `56b3d86`, matching WinePS4's documented revision. Its `docs/DEVELOPMENT.md` separately documents the ReXGlue/Renut/VideoOut dependency boundary. This makes WinePS4 and MP3 a particularly useful same-revision compatibility pair; MGR's `79fba0f`, TTT2's `ff30ff6` and ConkerLR's `87a517c` remain separate version targets. Godot is a confirmed direct consumer but its public build docs do not pin a kit commit.

The MGR and MP3 scripts are unusually valuable because they expose the exact
CMake hook names and the same explicit optional-component setting:

- MGR: `CMAKE_PROJECT_sa2_INCLUDE="$PS4_KIT/ps4-kit.cmake"`
- MP3: `CMAKE_PROJECT_mp3_INCLUDE="$PS4_KIT/ps4-kit.cmake"`
- Both pass `-DPS4_KIT_NETFS=OFF`.

This is a real shared interface: CMake includes the kit file immediately after
the matching `project(sa2 ...)` or `project(mp3 ...)` call. The kit must be
safe at that point in configuration, before the rest of the project's
CMakeLists has declared its executable target.

MGR's `mgr_ps4.cpp` callback only sets `log_tag = "mgr"`, so the kit must
initialize a shared default heap policy before invoking the callback. Godot's
`ps4_hooks.cpp` sets only `max_overflow_heaps = 8`; its comment specifies
512 MiB per direct-memory overflow heap. The branch now models this as a
provisional 512 MiB system-flexible main heap plus a 512 MiB default overflow
size when overflow is enabled. This is inferred from public source behavior,
not a recovered private struct ABI.

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
- The MP3 revision evidence comes from its current README rather than a private source checkout; it identifies the kit build used for the published release, not necessarily every historical MP3 build. No source or binary from the private kit has been recovered. Do not recreate
  private commit history or claim byte-for-byte compatibility.

## 2. Related ports that reuse the same graphics/runtime lineage

| Project | Relationship | What it can validate |
|---|---|---|
| [SSBU](https://github.com/iHaiDeeZ/SSBU) | Its build docs explicitly seed SMW's libc++ sysroot and Crema's Mesa; its VideoOut WSI says it descends from SMW/Resonance/Crema | native VideoOut WSI behavior, DXVK/RADV integration, runtime and package contracts; not proof of current private-kit consumption |
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | Public Clang 21/libc++ 21, GNM winsys and RADV baseline | toolchain, Mesa source/build and GNM primitive tests |
| [Crema](https://github.com/iHaiDeeZ/Crema) | Later GNM winsys with write-combined/cache-flush support | Mesa lifecycle and CPU/GPU cache coherency |
| [Resonance](https://github.com/iHaiDeeZ/Resonance) | Earlier VideoOut WSI and common runtime/build-tree ancestry | native VideoOut semantics and shared runtime provenance |
| [M33](https://github.com/iHaiDeeZ/M33), [SMW](https://github.com/iHaiDeeZ/SMW), [xemu-ps4](https://github.com/iHaiDeeZ/xemu-ps4), [TOTK-PS4](https://github.com/iHaiDeeZ/TOTK-PS4) | Consume or adapt the public Resonance/Crema/Dolphin/SMW build lineage | reproducible sysroot/Mesa bundle and native WSI integration |
| [ConkerLR-PS4](https://github.com/iHaiDeeZ/ConkerLR-PS4) | Direct private-kit consumer, separate revision `87a517c` | additional native-videoout, packaging and runtime-library build target |
| [Godot PS4 branch](https://github.com/iHaiDeeZ/godot/tree/ps4/platform/ps4) | Direct private-kit consumer through `ps4-kit.cmake` | large SCons-built engine linked as one grouped eboot; validates generic `ps4_kit_app()` and Mesa helpers |

WinePS4's notes explicitly say TTT2 and SSBU drew on the private kit's RADV
and VideoOut WSI, while Wine itself required a separate window-desktop mode.
That is why the native WSI must be a separate kit profile rather than being
folded into the Wine implementation.

## 3. Native VideoOut WSI contract recovered from public ports

The [SSBU PS4 WSI source](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_vk_wsi.cpp)
and [M33 technical notes](https://github.com/iHaiDeeZ/M33/blob/78f0f40edd55b6183631150cfe609170b5c530b8/docs/TECHNICAL.md)
expose a concrete native-videoout design. RADV is linked into the executable
and has no WSI; a separate layer supplies `VK_EXT_headless_surface` and
`VK_KHR_swapchain`.

Observed implementation contract:

- Open VideoOut and create a kernel event queue; register the flip event before
  creating the swapchain. The ports' boot notes warn that required system
  modules must be loaded before the first VideoOut call.
- Create linear `VK_FORMAT_R8G8B8A8_UNORM` images in memory that is
  device-local, host-visible and host-coherent. Derive row pitch and image
  offset from `vkGetImageSubresourceLayout`; do not assume tightly packed
  rows.
- Align VideoOut buffer addresses to 2 MiB and register the image addresses
  with `sceVideoOutRegisterBuffers`. The SSBU implementation advertises a
  fixed 1920x1080 surface and 2–3 images; these are profile choices, not
  universal Vulkan limits.
- Queue a GPU-side flip through `ac_ps4_submit_flip` after the frame's GPU
  work. Fall back to a CPU queue-idle followed by
  `sceVideoOutSubmitFlip` only if the winsys flip path fails.
- Track flip completion with `sceVideoOutGetFlipStatus` and the flip event
  queue so an image is not rendered into while it is still being scanned out.
  Alternate VideoOut buffer-index ranges during swapchain recreation, then
  wait idle and unregister the old buffer set.
- The cited SSBU port filters binary semaphores from submits but preserves
  timeline semaphores. Its comments report a PS4 device-loss issue when an
  otherwise-empty submit waits on or signals a binary semaphore. This is a
  platform-specific workaround that must be revalidated against the exact
  Mesa winsys and Vulkan workloads; it must not be generalized blindly.
- The WSI wraps ICD instance/device proc-address lookup and intercepts the
  surface, swapchain, acquire, present and semaphore/submit entry points. This
  is separate from RADV's own Vulkan ICD entry point.

This is sufficient to define the native profile's interface and a hardware
test plan, but not to transplant a file blindly. SSBU's source comments trace
the implementation through SMW, Resonance and Crema; record file-level
provenance and licensing before copying any implementation. Wine's
`window-desktop` WSI remains separate: it has window-sized CPU-readable
images and forwards completed frames to a desktop callback rather than owning
VideoOut.

## 4. Build-script topology: the kit is not the ReXGlue SDK

The current public MGR and MP3 build scripts show an important boundary that a
generic ps4_kit_app() wrapper would miss:

| Consumer | PS4 CMake source | Kit injection | Project SDK target | App target | Host-only work |
|---|---|---|---|---|---|
| MGR | $SRC/app | CMAKE_PROJECT_sa2_INCLUDE | rexcore from $SRC/sdk | sa2_eboot | ReXGlue host generator, XenosRecomp and shader tools |
| MP3 | $SRC/mp3/ps4 | CMAKE_PROJECT_mp3_INCLUDE | rexcore from $SRC/sdk | mp3_eboot | ReXGlue host generator, XenosRecomp and shader tools |
| TTT2 | $SRC/port/ps4 | no project-injection variable in the shown configure command | project runtime targets ttt2_win32, ttt2_platform, ttt2_runtime, ttt2_ffmpeg | ttt2_eboot | XenonRecomp, image tools, DXVK/vkd3d-shader and shader compilation |

MGR and MP3 each build their own PS4 ReXGlue SDK target before configuring the
application. Their scripts explicitly set REXGLUE_PLATFORM_PS4=ON,
REXGLUE_BUILD_HOST_TOOLS=OFF, REXGLUE_USE_VULKAN=ON,
REXGLUE_ENABLE_TRACY=OFF and REXGLUE_BUILD_TESTS=OFF for the target SDK.
The host code generator is built separately with the host Clang 21 compiler;
it must not inherit the PS4 target sysroot. MP3 additionally builds a separate
host XenosRecomp shader translator. The PS4 app build then injects the kit
CMake file and requests the eboot target.

This means the replacement kit must supply the shared PS4 environment,
toolchain/linker integration, runtime services, Mesa/RADV, native VideoOut
WSI, diagnostics/profiling and package tooling, while leaving the projects'
own ReXGlue SDK and host code-generation pipeline intact. Do not absorb
ReXGlue's SDK targets into the kit or treat them as kit runtime libraries.

TTT2 is a different integration shape. Its script configures port/ps4 and
has a libs mode that builds its four runtime libraries, then links
ttt2_eboot against stand-ins for generated game functions. This gives us a
useful intermediate acceptance test: target runtime libraries and a complete
application link can be tested before a user's game-specific code generation
is available. The script also builds FFmpeg's XMA/WMA decoder subset as a
project dependency; it is not the Wine image-decoder FFmpeg recipe.

## 5. Host configure regression test added

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

## 6. Public fork survey (13 November 2026)

The public fork search is useful, but it does **not** currently reveal another
clean-room reconstruction of the private kit. The surveyed results include
many mirrors and forks that are unchanged in the files relevant to the kit.
A fork count is not evidence of an independent build effort.

### DolphinPS4

GitHub listed 16 forks for the upstream repository at the time of the survey.
The following fork snapshots were inspected at file level:

| Fork | Relevant finding |
|---|---|
| [tehrzky/DolphinPS4](https://github.com/tehrzky/DolphinPS4) | Its BUILDING.md, toolchain/build-libcxx.sh, toolchain/build-runtime.sh, scripts/build-mesa.sh and port/mesa/ac_ps4_drm.c have the same blob SHAs as the other sampled snapshots. Its port/ps4_runtime.cpp is an older/different 29.5 KB variant (43240fd…) and lacks the newer executable-memory probing path present upstream. |
| [dynnocruel-gif/DolphinPS4](https://github.com/dynnocruel-gif/DolphinPS4) | The sampled build files match upstream; port/ps4_runtime.cpp is byte-identical to upstream at blob 29a060f…. |
| [shitastical/DolphinPS4](https://github.com/shitastical/DolphinPS4) | Same result as dynnocruel-gif for the sampled build files and runtime blob. |
| [Mrexel55/dolphinps4](https://github.com/Mrexel55/dolphinps4) | Repository is only a tiny stub (size 1); no useful kit implementation surfaced. |
| [hikaru457/DolphinPS4-sdhc-and-wii-disc-load](https://github.com/hikaru457/DolphinPS4-sdhc-and-wii-disc-load) | A feature fork; not yet shown to contain independent kit/build reconstruction. |

The useful result is not a second kit: it is an **older runtime snapshot** in
tehrzky's fork that can be compared against current upstream to understand when
the executable-memory probe was introduced. Do not treat that fork as a more
complete or more buildable kit.

### Other lineage forks sampled

Targeted file checks also found identical blob SHAs between upstream and the
sampled forks for:
- Crema: port/ps4_runtime.cpp (5652434…) in upstream and the checked
  forks (Glec6, lanvas2008, wagnerguerchee).
- Resonance: port/ps4_runtime.cpp (377d553…) in upstream and the checked
  forks (tehrzky, Glec6).
- M33: port/ps4_runtime.cpp (b4fe309…) in upstream and
  leecloudvictor/ps4-M33.
- SMW: scripts/make-sdk.sh (84f8e9a…) in upstream and ayasns/SMW.
- TTT2-PS4: README.md (d00d041…) in upstream and Stayhye/TTT2-PS4.

These are targeted blob comparisons, not full recursive tree comparisons.
They show no divergence in the sampled runtime/build entry points; they do
not prove that every fork file or branch is identical.

### WinePS4 and interpretation

The public fork search found the user's Nabouna32/WinePS4 fork alongside
upstream; no other public WinePS4 fork surfaced in the search. Among the
sampled DolphinPS4, Crema, Resonance, M33, SMW and TTT2 forks, no independent
ps4-kit replacement, compatibility shim, or reproducible build recipe was
found.

This is a bounded negative result: GitHub search can miss private repositories,
deleted forks, unindexed branches and renamed copies. Continue checking
fork-specific commits and branches when a new candidate appears. For now,
public forks are useful mainly as historical snapshots, not as a recovered
private kit.

## 7. Next implementation gates

1. [x] Add the host injection test to `.github/workflows/ps4-kit-host-tests.yml`;
   its first GitHub Actions result still needs to be checked.
2. Audit the full MGR and MP3 patched CMakeLists and generated link commands;
   identify which target receives runtime, RADV archives, ICD entry point,
   VideoOut WSI and system-library imports.
3. Implement the native-videoout WSI as a separate profile from the recovered
   contract above, with source provenance and lifecycle tests. Keep
   `PS4_KIT_NETFS=OFF` explicit and avoid implementing or advertising NetFS
   until its required symbols and semantics are known.
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


## 8. TTT2's additional runtime ABI and CMake contract

The TTT2 patch is now inspected directly from its patch blob (the file is too
large for the ordinary file-content endpoint). Its PS4 CMake project:

- sets project name ttt2_ps4 and executable target ttt2;
- includes $HOME/ps4-kit/ps4-kit.cmake;
- expects PS4_KIT_DIR to be the root of the kit and adds
  $PS4_KIT_DIR/runtime to its platform include path;
- calls ps4_kit_link_mesa(ttt2) then ps4_kit_app(ttt2);
- links the kit's diagnostics API through its platform glue.

The compatibility entry point now exports PS4_KIT_DIR and PS4_KIT_ROOT. The
helper implementation supports both observed call orders: Wine calls
ps4_kit_app before ps4_kit_link_mesa, while the ReXGlue/Godot/TTT2 paths call
Mesa first.

The TTT2 patch also requires the diagnostics functions listed in
DIAGNOSTICS-SAMPLER-TRACE.md. Those are not implemented yet, so TTT2 remains a
confirmed direct consumer but not a currently linkable target.
