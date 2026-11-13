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
| [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4) | `README.md`, `CREDITS.md`, `scripts/build-mgr.sh` | `79fba0f`; sources `toolchain/env.sh`, injects `ps4-kit.cmake` through `CMAKE_PROJECT_sa2_INCLUDE`, sets `PS4_KIT_NETFS=OFF`; the project credits kit runtime, RADV, VideoOut, crash log, profiler and packaging | **native-videoout**: app-owned VideoOut presentation, ReXGlue PS4 SDK integration, RADV and package creation |

The two revisions are different. The replacement must therefore be a versioned,
documented compatibility layer rather than a one-off snapshot tuned only to
WinePS4. The exact private implementation and complete historical API for
`79fba0f` remain unavailable; do not invent undocumented function signatures.
The first integration target is to make both projects' public build entry points
configure against one shared kit, then close their distinct link/runtime needs.

## 2. Projects that supply or reuse the common base

| Project | Relationship established by its public build/technical docs | Reusable kit layer |
|---|---|---|
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | Foundational public OpenOrbis/Clang 21 + libc++ 21 and Mesa 26.0.8 GNM/RADV build; its MIT/per-file GNM winsys is already staged in this reconstruction | Toolchain reference, Mesa/GNM baseline, native VideoOut WSI reference; audit per-file licences before reuse |
| [Crema](https://github.com/iHaiDeeZ/Crema) | Cemu PS4 port; supplies a later GNM winsys variant and Mesa build tree used by other projects | Mesa/RADV build inputs and native VideoOut runtime reference; its cache-coherency additions are relevant to Wine placed mappings |
| [Resonance](https://github.com/iHaiDeeZ/Resonance) | Dreamcast/Flycast PS4 port; its build tree is the seed for M33 and the common runtime ancestry | Shared runtime, heap, diagnostics, VideoOut WSI, sysroot and build-tree conventions |
| [M33](https://github.com/iHaiDeeZ/M33) | `docs/TECHNICAL.md` says its `port/` runtime and toolchain come from Resonance; `seed-build-tree.sh` brings in Resonance's sysroot/Mesa plus RPCS3 FFmpeg 7.1.1 | Compatibility tests for runtime, memory arenas, VideoOut and packaging; FFmpeg artifact provenance |
| [SMW](https://github.com/iHaiDeeZ/SMW) | `docs/BUILDING.md` uses Resonance's build tree and Crema's Mesa build; release `sdk-1` bundles a sysroot and static Mesa archives | Reproducible SDK/sysroot artifact format and common RADV archive bundle |
| [TOTK-PS4](https://github.com/iHaiDeeZ/TOTK-PS4) | Its build guide seeds its work tree from SMW's build tree | Validate that the same SDK bundle/build-tree contract works for another large static recompilation |

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
     (DolphinPS4/Crema/Resonance/M33/SMW/TOTK/MGR-style apps);
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
2. Add a second compatibility test for MGR's public CMake injection path with
   `PS4_KIT_NETFS=OFF`; identify the exact native VideoOut symbols and package
   hooks before implementing them.
3. Extract the common host toolchain/sysroot/Mesa manifest into a versioned
   SDK profile, using SMW's published `sdk-1` format as a reproducibility
   reference rather than copying opaque binaries blindly.
4. Add build-tree smoke profiles for DolphinPS4/Crema/Resonance, then M33 and
   SMW/TOTK. Record which ones build from source and which consume a prebuilt
   SDK bundle.
5. Only call the kit multi-project compatible when each profile has a clean
   configure, link, package and (where hardware is available) runtime test.

No proprietary files, game dumps, keys or private SDK binaries belong in this
reconstruction. The goal is an independent, source-auditable toolchain/runtime
that lets contributors build lawful public project sources and work on them.
