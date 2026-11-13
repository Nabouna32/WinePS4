# PS4 kit — public project compatibility matrix

This matrix widens the reconstruction target from WinePS4 alone to the public
PS4 projects that either name the unpublished `ps4-kit` or reuse the same
toolchain, Mesa/RADV build and runtime lineage. It deliberately distinguishes
**direct kit consumers** from projects that consume upstream build artifacts or
carry an earlier copy of the shared runtime. A shared dependency is not proof
that a project calls the private kit API.

## 1. Direct consumers of the unpublished kit

| Project | Public evidence | Kit revision / interface visible | Build profile that must be supported |
|---|---|---|---|
| [WinePS4](https://github.com/iHaiDeeZ/WinePS4) | `docs/BUILDING.md`, `app/CMakeLists.txt`, `app/launcher/CMakeLists.txt`, probes | `56b3d86`; `ps4_cmake`, `ps4-kit.cmake`, `ps4_kit_app()`, `ps4_kit_link_mesa()`, `PS4_LINK_SCRIPT`, `PS4_SYSROOT`, `make-pkg.sh` | **window-desktop**: headless Vulkan surface, window-sized host-readable swapchain images and callback to the desktop; no app-owned VideoOut |
| [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4) | `README.md`, `CREDITS.md`, `scripts/build-mgr.sh` | `79fba0f`; sources `toolchain/env.sh`, injects `ps4-kit.cmake` through `CMAKE_PROJECT_sa2_INCLUDE`, sets `PS4_KIT_NETFS=OFF`; builds `sa2_eboot` and uses kit packaging | **native-videoout**: ReXGlue PS4 SDK, RADV and app-owned VideoOut |
| [MP3-PS4](https://github.com/iHaiDeeZ/MP3-PS4) | `README.md`, `CREDITS.md`, `docs/DEVELOPMENT.md`, `scripts/build-mp3.sh` | `56b3d86`; injects `ps4-kit.cmake` through `CMAKE_PROJECT_mp3_INCLUDE`, sets `PS4_KIT_NETFS=OFF`; builds `mp3_eboot` and uses kit packaging | **native-videoout**: ReXGlue PS4 SDK, RADV and app-owned VideoOut |
| [TTT2-PS4](https://github.com/iHaiDeeZ/TTT2-PS4) | `README.md`, `CREDITS.md`, `scripts/build-ttt2.sh` | `ff30ff6`; sources `toolchain/env.sh`, uses `ps4_cmake` on `port/ps4` and `make-pkg.sh`; credits kit runtime, RADV and VideoOut | **native-videoout**: XenonRecomp/DXVK, app-owned VideoOut and package creation |
| [ConkerLR-PS4](https://github.com/iHaiDeeZ/ConkerLR-PS4) | `README.md`, `CREDITS.md`, `scripts/build-clx.sh` | `87a517c`; sources `~/ps4-kit/toolchain/env.sh`, uses kit `make-pkg.sh`; has a `libs` runtime-only build mode | **native-videoout**: Xbox recomp runtime, RADV, VideoOut and packaging |
| [Godot PS4 branch](https://github.com/iHaiDeeZ/godot/tree/ps4/platform/ps4) | `platform/ps4/README.md`, `build.sh`, `eboot/CMakeLists.txt` | includes `$PS4_KIT/ps4-kit.cmake`, calls `ps4_kit_link_mesa()` and `ps4_kit_app()`; no public commit pin found | **native-videoout**: SCons builds engine libs, CMake links a grouped eboot |

WinePS4 and MP3-PS4 both document kit revision `56b3d86`; MGR, TTT2 and ConkerLR document the distinct revisions `79fba0f`, `ff30ff6` and `87a517c`. Godot is a direct consumer with no public revision pin found. The replacement must therefore be a versioned,
documented compatibility layer rather than a one-off snapshot tuned only to
WinePS4. The exact private implementation and complete historical APIs remain unavailable;
do not invent undocumented function signatures. MGR and MP3 expose the same
CMake project-injection pattern with different project names and both disable
the optional NetFS component. TTT2 uses a different CMake path but the same
environment/package boundary. See [DIRECT-CONSUMER-AUDIT.md](DIRECT-CONSUMER-AUDIT.md)
for the source-level details and the new host configure regression test.

## 2. Projects that supply or reuse the common base

| Project | Relationship established by its public build/technical docs | Reusable kit layer |
|---|---|---|
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | Foundational public OpenOrbis/Clang 21 + libc++ 21 and Mesa 26.0.8 GNM/RADV build; its MIT/per-file GNM winsys is already staged in this reconstruction | Toolchain reference, Mesa/GNM baseline, native VideoOut WSI reference; audit per-file licences before reuse |
| [Crema](https://github.com/iHaiDeeZ/Crema) | Cemu PS4 port; supplies a later GNM winsys variant and Mesa build tree used by other projects | Mesa/RADV build inputs and native VideoOut runtime reference; its cache-coherency additions are relevant to Wine placed mappings |
| [Resonance](https://github.com/iHaiDeeZ/Resonance) | Dreamcast/Flycast PS4 port; its build tree is the seed for M33 and other later ports | Shared runtime, heap, diagnostics, VideoOut WSI, sysroot and build-tree conventions |
| [M33](https://github.com/iHaiDeeZ/M33) | `docs/TECHNICAL.md` says its `port/` runtime and toolchain come from Resonance; `seed-build-tree.sh` brings in Resonance's sysroot/Mesa plus RPCS3 FFmpeg 7.1.1 | Compatibility tests for runtime, memory arenas, VideoOut and packaging; FFmpeg artifact provenance |
| [xemu-ps4](https://github.com/iHaiDeeZ/xemu-ps4) | Its build guide seeds `xemups4-build` from Resonance's tree, then builds a project-specific libc overlay, Vulkan headers, pixman, glib and SDL3 | Reusable sysroot/Mesa seed plus a test of the kit's ability to support additional host/target dependency recipes |
| [SMW](https://github.com/iHaiDeeZ/SMW) | `docs/BUILDING.md` uses Resonance's build tree and Crema's Mesa build; release `sdk-1` bundles a sysroot and static Mesa archives | Reproducible SDK/sysroot artifact format and common RADV archive bundle |
| [TOTK-PS4](https://github.com/iHaiDeeZ/TOTK-PS4) | Its build guide seeds its work tree from SMW's build tree | Validate that the same SDK bundle/build-tree contract works for another large static recompilation |
| [SSBU](https://github.com/iHaiDeeZ/SSBU) | Its build docs use SMW's sysroot and Crema's Mesa; its WSI source documents the Resonance/Crema lineage | Native VideoOut WSI, DXVK/RADV and package compatibility; public tree carries its own runtime copy |
| [PS4Liberty](https://github.com/iHaiDeeZ/PS4Liberty) | Discontinued independent port; uses DolphinPS4 toolchain/RADV and its own runtime | 512 MiB flexible heap, direct-memory overflow, 4.5 GiB guest window and native VideoOut reference |
| [HENkaku-PS4](https://github.com/iHaiDeeZ/HENkaku-PS4) and [Conker-PS4](https://github.com/iHaiDeeZ/Conker-PS4) | Later integrated public runtime based on Resonance/DolphinPS4/M33/Crema | Regression references for allocator, diagnostics, VideoOut and sysroot |
| [OpenCE-PS4](https://github.com/iHaiDeeZ/OpenCE-PS4), [Shattered Pixel Dungeon PS4](https://github.com/iHaiDeeZ/shattered-pixel-dungeon-ps4), [Mari0-PS4](https://github.com/iHaiDeeZ/mari0-ps4), [SRB2-PS4](https://github.com/iHaiDeeZ/SRB2-PS4) | Carry or consume public runtime lineage from SMW/DolphinPS4/love-ps4/DeCast-S4 | Additional platform consumers and older-runtime regression targets |

These are not all confirmed direct consumers of the private kit API. They are
nevertheless essential compatibility targets because they show which common
pieces the kit was assembled from and which build artifacts are already shared.

## 3. Architecture required for a genuinely reusable kit

Do not force every project through one presentation backend. Keep the common
substrate shared, and make project-facing profiles explicit:

1. **Host toolchain** — host LLVM/Clang, LLD, CMake, Ninja, Meson, Python,
   shader tools and package utilities. Host tools must never be confused with
   target sysroot libraries.
2. **PS4 target toolchain and sysroot** — OpenOrbis headers, libc overlay,
   libc++/libc++abi, compiler wrappers, linker script and SDK utilities. Pin
   versions per profile; the public modern ports use LLVM 21, while older
   PacBrew packages also contain Clang 12 and must not shadow it.
3. **Common runtime** — heap and direct-memory policy, thread wrappers,
   module loading, crash/boot diagnostics and profiler. Keep project-specific
   policies configurable; Wine's direct-first/overflow heap contract is not
   interchangeable with Dolphin/M33's allocator policy.
4. **Common RADV/GNM driver** — one audited Mesa source/patch/build and archive
   manifest, with the Crema cache-coherency delta tracked explicitly. A Mesa
   archive bundle is reusable only when its source revision, target ABI and
   configuration match.
5. **Presentation profiles** — at minimum:
   - `native-videoout`: app owns VideoOut, swapchain buffers and flip queue
     (DolphinPS4/Crema/Resonance/M33/xemu-ps4/SMW/TOTK/SSBU/MGR/MP3/TTT2-style apps);
   - `window-desktop`: Wine program has no VideoOut; its headless swapchain
     is copied/forwarded to the desktop callback (WinePS4).
   These backends share RADV/GNM primitives but not surface ownership or
   present semantics.
6. **Packaging profile** — common GP4/SFO/PKG machinery with explicit title,
   content ID, attributes, optional SPRX and staged-file manifests.
7. **Project adapters and lockfiles** — a small adapter for each project
   records its required kit API, WSI profile, sysroot/Mesa revisions,
   dependencies and package recipe. Do not make each app silently inherit
   whichever Mesa or LLVM happens to be installed.

## 4. Dependency/version constraints already established

- OpenOrbis/PacBrew is the public SDK/tool distribution; do not reconstruct or
  redistribute proprietary Sony SDK content.
- The modern DolphinPS4/Crema/Resonance/M33 toolchain lineage uses LLVM 21 and
  libc++ 21. WinePS4's private kit revision is not yet proven to use precisely
  the same compiler revision.
- PacBrew's Clang 12, FreeType 2.10.1 and FFmpeg 5.0 are not replacements for
  the LLVM 21 / FreeType 2.13.3 / FFmpeg 7.1.1 versions documented by the
  relevant modern builds.
- WinePS4 needs its own FreeType 2.13.3, FFmpeg 7.1.1 image-only build and
  DXVK 3.1.1 inputs. These are project dependencies, not generic kit runtime.
- M33's FFmpeg 7.1.1 artifacts come from the RPCS3 PS4 build tree. SMW's
  `sdk-1` release is a useful sysroot/RADV artifact, but neither artifact
  should be accepted without checking hashes, source revisions and ABI.
- Keep licences per file and per upstream component. In particular, do not
  treat a repository's top-level licence as automatically covering vendored
  Mesa winsys, dlmalloc, runtime adapters or generated patches.

## 5. Work order and acceptance

1. Finish WinePS4's current unchanged-source build path first: environment,
   launcher/app configure, RADV window-WSI link, eboot and package.
2. Add host configure regression tests for MGR's `CMAKE_PROJECT_sa2_INCLUDE`
   and MP3's `CMAKE_PROJECT_mp3_INCLUDE` paths with `PS4_KIT_NETFS=OFF`;
   identify the exact native VideoOut symbols and package hooks before implementing them.
3. Extract the common host toolchain/sysroot/Mesa manifest into a versioned
   SDK profile, using SMW's published `sdk-1` format as a reproducibility
   reference rather than copying opaque binaries blindly.
4. Add build-tree smoke profiles for DolphinPS4/Crema/Resonance, then M33,
   xemu-ps4, SMW, SSBU and TOTK; audit TTT2's separate CMake path. Record which ones build from source and which
   consume a prebuilt SDK bundle.
5. Only call the kit multi-project compatible when each profile has a clean
   configure, link, package and (where hardware is available) runtime test.

No proprietary files, game dumps, keys or private SDK binaries belong in this
reconstruction. The goal is an independent, source-auditable toolchain/runtime
that lets contributors build lawful public project sources and work on them.


## 6. Current branch readiness (2026-11-13)

| Consumer | CMake/API compatibility reconstructed | Remaining hard blocker |
|---|---|---|
| WinePS4 | Host CMake smoke test; app-before-Mesa helper order; window-desktop profile | Target heap backend, target runtime validation and full target Mesa/eboot/PKG build |
| MGR-PS4 | Project injection hook and Mesa-before-app helper order | Native VideoOut WSI, real target heap/runtime and ReXGlue target build |
| MP3-PS4 | Project injection hook and Mesa-before-app helper order | Native VideoOut WSI, real target heap/runtime and ReXGlue target build |
| TTT2-PS4 | PS4_KIT_DIR export, native profile selection and project CMake path recovered | Native VideoOut WSI; diagnostics symbols now have a first-pass implementation but need full stack unwinding, address histograms and target validation |
| ConkerLR-PS4 | Kit environment/CMake integration and libs-only build path recovered | Native VideoOut WSI and target runtime |
| Godot PS4 | Direct ps4-kit.cmake inclusion, Mesa/app helper calls and default heap callback recovered | Native VideoOut WSI and target runtime |

The helper order is deliberately order-independent: Wine calls ps4_kit_app()
before ps4_kit_link_mesa(), while MGR, MP3, TTT2, ConkerLR and Godot call
Mesa first. The CMake layer waits until both helpers have run before attaching
the selected WSI source.

The host CI workflow now includes tests for both helper orders and distinct
WSI profiles, plus the C/C++ exported diagnostics-directory symbol. These
tests do not constitute a target compile. Native VideoOut is intentionally
missing and fails configuration explicitly rather than receiving the Wine
window WSI by mistake.
