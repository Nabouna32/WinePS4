# Recursive credit and repository lineage audit

Date: 2026-11-13

Status: public-source audit. This records repositories and build contracts that can
be inspected; it does not claim that every cited project directly consumes the
unpublished private `ps4-kit`.

## 1. Primary finding: the kit's own credits describe its ancestry

WinePS4's public `CREDITS.md` says that `ps4-kit` is ShiroKlein's shared PS4
base and explicitly names Resonance, DolphinPS4, M33 and Crema as source
projects. Its scope is runtime, heap, threads, RADV port, window system and
packaging. WinePS4's `docs/BUILDING.md` pins the release build to kit revision
`56b3d86`.

This makes the four repositories the highest-priority source lineage, not
merely optional examples. The credits and source comments lead one generation
further back to [love-ps4](https://github.com/Mari0/love-ps4), the first working
PS4 heap and thread-runtime reference. The `iHaiDeeZ/love-ps4` repository also
contains the source files used by later ports. `SSBU` separately credits
**PSChrome** for the random source and early malloc fixes; its standalone
source repository was not found publicly, but downstream implementations are
available in SSBU and DolphinPS4.

- [Resonance](https://github.com/iHaiDeeZ/Resonance)
- [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)
- [M33](https://github.com/iHaiDeeZ/M33)
- [Crema](https://github.com/iHaiDeeZ/Crema)

The source tree, commit history, patch baselines and build scripts in those
repositories should be compared before implementing equivalent kit APIs.

## 2. Runtime and allocator lineage

### love-ps4 (the earlier runtime)

The LÖVE port provides the earliest concrete PS4 heap implementation in this
lineage: `src/common/ps4_heap.cpp` queries the actual flexible-memory pool,
tries system-flexible mspaces, then falls back to regular flexible memory while
preserving room for Piglet. `src/common/ps4.cpp` implements
`__cxa_thread_atexit_impl` through a pthread-key destructor list. Its source
carries the LÖVE zlib-style notice, which must be retained and modified files
must be marked. The later ports explicitly say their runtime fixes are based
on these two files.

- [LÖVE for PS4 source mirror](https://github.com/iHaiDeeZ/love-ps4)
- [Original LÖVE for PS4 project](https://github.com/Mari0/love-ps4)
- [Heap implementation](https://github.com/iHaiDeeZ/love-ps4/blob/main/src/common/ps4_heap.cpp)
- [Thread destructor and system-module setup](https://github.com/iHaiDeeZ/love-ps4/blob/main/src/common/ps4.cpp)

### Resonance / DolphinPS4

At the pinned public revisions, both repositories contain:

- `port/ps4_dlmalloc.c`: dlmalloc configured for mspaces only
  (`ONLY_MSPACES`), with no mmap/sbrk growth path; the adapter maps and manages
  memory itself, and includes locking and footers.
- `port/ps4_runtime.cpp`: documents initialization before `main()`, linker
  `--wrap` redirection of the malloc family, and why dlmalloc replaced
  Sony's `sceLibcMspace`.
- `toolchain/runtime/ps4_malloc.c`: an earlier/smaller allocator shim still
  declaring Sony `sceLibcMspace*` entry points.

The stated failure is concrete: Sony's mspace stopped returning allocations
after roughly 800 small allocations during Dolphin static initialization,
with only about 49 KiB used in a 1 GiB heap. This is stronger evidence than a
generic preference for dlmalloc.

Relevant pinned source:
- [Resonance allocator](https://github.com/iHaiDeeZ/Resonance/blob/6b87389033d4f239e97d36c605bf7eea384d1e23/port/ps4_dlmalloc.c)
- [Resonance runtime](https://github.com/iHaiDeeZ/Resonance/blob/6b87389033d4f239e97d36c605bf7eea384d1e23/port/ps4_runtime.cpp)
- [DolphinPS4 allocator](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_dlmalloc.c)
- [DolphinPS4 credits](https://github.com/iHaiDeeZ/DolphinPS4/blob/main/CREDITS.md)

**Reconstruction implication:** dlmalloc's upstream allocator source is
identified as CC0 in DolphinPS4 credits, but the PS4 adapter is separate
project code and must retain its own applicable license/notice. Do not assume
the adapter is CC0 merely because dlmalloc is. The new kit backend should use
a clean-room adapter around the appropriately licensed allocator source, with
explicit mapping ownership, synchronization and rollback tests.

### PSChrome (unpublished or not located in this pass)

SSBU's credits and build scripts identify PSChrome as the source of its
random-number implementation and earlier malloc replacement. The local build
path `~/pschrome-build` appears in `toolchain/build-libcxx.sh`. The source
snapshot retained in SSBU is
[`toolchain/runtime/ps4_malloc.c`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/toolchain/runtime/ps4_malloc.c);
its CRT-level replacement evolved into the `_sceLibcMallocReplace` process
parameter table described in
[`port/ps4_crt1.S`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/port/ps4_crt1.S).
No public standalone PSChrome repository was identified, so treat these as
downstream snapshots and keep the original attribution.

### HENkaku-PS4

HENkaku's credits independently identify the same four-project PS4 runtime
lineage and say that it includes heap, crash/boot logs, profiler, DNS and
VideoOut presentation. Its build guide seeds the sysroot from M33 and takes a
RADV/GNM Mesa build from the Crema/TOTK build tree.

Relevant sources:
- [HENkaku credits](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/main/CREDITS.md)
- [HENkaku build guide](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/main/docs/BUILDING.md)
- [HENkaku port notes](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/main/docs/PORT.md)
- [HENkaku allocator](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/945f186a441dd01beddf1b9e389dd44cbbf33c69/port/ps4_dlmalloc.c)
- [HENkaku runtime](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/945f186a441dd01beddf1b9e389dd44cbbf33c69/port/ps4_runtime.cpp)

HENkaku is a valuable independent integration check, but not proof that its
current APIs match WinePS4's older private kit revision.

### Conker-PS4 and G-Diffuser-PS4

Conker's credits identify HENkaku-PS4 and G-Diffuser-PS4 as sources for the
PS4 runtime, toolchain, VideoOut WSI and pipeline-cache design. Its developer
guide explicitly attributes its `port/` runtime and `toolchain/` to HENkaku,
and its `ps4/ps4_wsi.cpp` to HENkaku. The guide also records a real scheduling
failure: dlmalloc's spin/yield lock could starve when held by a low-priority
shader thread; the port changed the lock to a mutex.

- [Conker credits](https://github.com/iHaiDeeZ/Conker-PS4/blob/main/CREDITS.md)
- [Conker developer guide](https://github.com/iHaiDeeZ/Conker-PS4/blob/main/docs/DEVELOPMENT.md)
- [G-Diffuser-PS4](https://github.com/iHaiDeeZ/G-Diffuser-PS4)

G-Diffuser-PS4 is therefore a source to inspect for the VideoOut WSI and
pipeline cache, even though its public README does not declare it a direct
ps4-kit consumer.

## 3. Toolchain and sysroot lineage

### OpenOrbis and PacBrew

The projects distinguish the upstream OpenOrbis toolchain from PacBrew's
distribution of it. The later ports use PacBrew's OpenOrbis installation at
`/opt/pacbrew/ps4/openorbis`, with clang 21 and libc++ 21 in a separate PS4
sysroot. The toolchain distribution is not the sysroot and neither is the
RADV build.

Repositories to audit as a set:
- [OpenOrbis organization](https://github.com/OpenOrbis)
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- [PacBrew ps4-openorbis](https://github.com/PacBrew/ps4-openorbis)
- [PacBrew ps4-openorbis-portlibs](https://github.com/PacBrew/ps4-openorbis-portlibs)
- [PacBrew ps4-openorbis-musl](https://github.com/PacBrew/ps4-openorbis-musl)
- [PacBrew ps4-openorbis-utils](https://github.com/PacBrew/ps4-openorbis-utils)

The old PacBrew clang 12 packages and newer clang 21/libc++ 21 sysroot are
different generations. Do not silently mix their headers, libc++ ABI, linker
flags or libraries.

### FFmpeg

M33's technical guide says its FFmpeg 7.1 libraries come from the RPCS3
project's PS4 build and lists the codec subset used. HENkaku's build guide
then seeds its sysroot from M33, and its credits explicitly attribute FFmpeg
7.1 and the sysroot libraries to M33. WinePS4 is different: it builds FFmpeg
7.1.1 image decoders only, from the RPCS3 build tree, via the PS4 compiler
wrappers. MGR-PS4's ReXGlue FFmpeg subset is a separate project-specific
dependency.

- [M33 technical/build guide](https://github.com/iHaiDeeZ/M33/blob/main/docs/TECHNICAL.md)
- [HENkaku build guide](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/main/docs/BUILDING.md)
- [WinePS4 build guide](https://github.com/iHaiDeeZ/WinePS4/blob/main/docs/BUILDING.md)

The exact RPCS3 PS4 build recipe, source revision, patch set and artifact
manifest still need to be recovered. Do not substitute PacBrew's older
FFmpeg package for the RPCS3 FFmpeg 7.1/7.1.1 builds.

## 4. Other PS4 projects in the same developer ecosystem

These repositories are useful compatibility and regression targets, but
their public build descriptions do not establish that they all consume the
same private ps4-kit revision:

| Repository | Publicly documented platform integration | Classification |
|---|---|---|
| [WinePS4](https://github.com/iHaiDeeZ/WinePS4) | private kit `56b3d86`; window-desktop WSI | Direct consumer |
| [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4) | private kit `79fba0f`; ReXGlue CMake injection; native VideoOut | Direct consumer |
| [MP3-PS4](https://github.com/iHaiDeeZ/MP3-PS4) | private kit `56b3d86`; ReXGlue CMake injection; native VideoOut | Direct consumer |
| [TTT2-PS4](https://github.com/iHaiDeeZ/TTT2-PS4) | private kit `ff30ff6`; separate CMake integration | Direct consumer |
| [ConkerLR-PS4](https://github.com/iHaiDeeZ/ConkerLR-PS4) | private kit `87a517c`; `scripts/build-clx.sh` sources `~/ps4-kit/toolchain/env.sh` and packages through kit scripts | Direct consumer |
| [Godot PS4 branch](https://github.com/iHaiDeeZ/godot/tree/ps4/platform/ps4) | `platform/ps4/eboot/CMakeLists.txt` includes `$PS4_KIT/ps4-kit.cmake`, links Mesa and calls `ps4_kit_app()` | Direct consumer |
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | shared runtime, GNM/RADV, native VideoOut | Core source lineage |
| [Resonance](https://github.com/iHaiDeeZ/Resonance) | shared runtime, allocator, WSI, toolchain | Core source lineage |
| [M33](https://github.com/iHaiDeeZ/M33) | Resonance runtime/WSI, RPCS3 FFmpeg | Core source lineage |
| [Crema](https://github.com/iHaiDeeZ/Crema) | RADV/GNM, runtime, native VideoOut | Core source lineage |
| [HENkaku-PS4](https://github.com/iHaiDeeZ/HENkaku-PS4) | later integrated runtime and sysroot | Later integration/reference |
| [Conker-PS4](https://github.com/iHaiDeeZ/Conker-PS4) | HENkaku runtime, WSI and toolchain | Later integration/reference |
| [PS4Liberty](https://github.com/iHaiDeeZ/PS4Liberty) | discontinued; DolphinPS4 sysroot/RADV plus its own 512 MiB flexible heap and direct-memory overflow heaps | Independent runtime reference |
| [Shattered Pixel Dungeon PS4](https://github.com/iHaiDeeZ/shattered-pixel-dungeon-ps4) | DolphinPS4 runtime, dlmalloc, clang 21; love-ps4 SDL2 patch | Independent runtime consumer/reference |
| [Mari0-PS4](https://github.com/iHaiDeeZ/mari0-ps4) | runs on the LÖVE for PS4 engine | LÖVE runtime consumer |
| [SRB2-PS4](https://github.com/iHaiDeeZ/SRB2-PS4) | runtime attributed to DeCast-S4 / love-ps4; PacBrew clang 12 | Legacy runtime reference |
| [G-Diffuser-PS4](https://github.com/iHaiDeeZ/G-Diffuser-PS4) | HENkaku-derived runtime/WSI lineage | Later integration/reference |
| [SMW](https://github.com/iHaiDeeZ/SMW) | Resonance/Crema toolchain, VideoOut WSI, lazy guest memory | Later integration/reference |
| [PS4SX2](https://github.com/iHaiDeeZ/PS4SX2) | CEMUPS4 toolchain/runtime and sysroot; PS5SX2 console layer | Separate integration/reference |
| [xemu-ps4](https://github.com/iHaiDeeZ/xemu-ps4) | to be audited | Candidate |
| [SSBU](https://github.com/iHaiDeeZ/SSBU) | native VideoOut WSI research/reference | Candidate |
| [BOTW](https://github.com/iHaiDeeZ/BOTW) | to be audited | Candidate |
| [TOTK-PS4](https://github.com/iHaiDeeZ/TOTK-PS4) | Crema RADV/GNM lineage; build artifacts | Candidate |

PS4SX2's current `ps4/build-ps4.sh` explicitly uses a read-only
`$HOME/cemups4` runtime and `$HOME/cemups4-build` sysroot/RADV build. Its
`Makefile.ps4` specifies clang 21, libc++ 21 and the PS5SX2 `coreorbis`
layer. This is evidence for a parallel kit-like base, not proof that
CEMUPS4 is the unpublished ps4-kit or that it can be substituted for it.

### ConkerLR-PS4 and Godot: two more confirmed direct consumers

ConkerLR-PS4's README pins its build to private kit revision `87a517c`; its
build script sources `~/ps4-kit/toolchain/env.sh` and invokes the kit package
script. Godot's `ps4` branch includes `$PS4_KIT/ps4-kit.cmake` directly from
its eboot CMake project, calls `ps4_kit_link_mesa()` and `ps4_kit_app()`, and
uses the kit's VideoOut presenter. Godot's build script runs SCons first and
then links the PS4 eboot through `ps4_cmake`/Ninja. Its README does not pin a
kit commit, so record its tested revision only after recovering the build
state or a release log.

### Other useful but non-kit runtime ports

- [PS4Liberty](https://github.com/iHaiDeeZ/PS4Liberty) is explicitly
  discontinued but published as a reference. Its PS4 README describes a
  separate runtime with a 512 MiB flexible-memory heap plus direct-memory
  overflow heaps, a 4.5 GiB guest window, a VideoOut WSI and DolphinPS4's
  toolchain/RADV build. It is not a direct private-kit consumer.
- [Shattered Pixel Dungeon PS4](https://github.com/iHaiDeeZ/shattered-pixel-dungeon-ps4)
  credits DolphinPS4's `ps4_runtime.cpp`, `ps4_string.c` and `ps4_dlmalloc.c`,
  plus the love-ps4 SDL2 patch. This is another downstream check of the public
  runtime, not the private kit.
- [Mari0-PS4](https://github.com/iHaiDeeZ/mari0-ps4) runs on the LÖVE for PS4
  engine. [SRB2-PS4](https://github.com/iHaiDeeZ/SRB2-PS4) attributes its
  runtime to DeCast-S4 / love-ps4. A standalone public DeCast-S4 repository
  was not located in this pass; keep it as an unresolved upstream lead.

## 5. What this changes in the reconstruction plan

1. Treat love-ps4, Resonance, DolphinPS4, M33 and Crema as the primary
   implementation lineage; compare exact commits and file history. Include the
   PSChrome downstream malloc/CRT snapshots when auditing process-wide allocation.
2. Use HENkaku, Conker, ConkerLR and Godot as later regression references for heap locking,
   runtime initialization, diagnostics and VideoOut behavior.
3. Keep two WSI profiles separate:
   - Wine's `window-desktop`: headless RADV images presented to the desktop
     callback;
   - emulator `native-videoout`: VideoOut-registered linear images and
     flip-completion tracking.
4. Recover the full build inputs, not just binaries: PacBrew package versions,
   clang/libc++ sysroot, Mesa commit and patch set, Vulkan-Headers, FFmpeg
   source/revision, and package tools.
5. Keep each consumer's emulator/recompiler and host-side generators in its
   own project. The kit supplies the shared PS4 platform/runtime, not a
   replacement for ReXGlue, PCSX2, PPSSPP, Cemu, Wine or the recompilers.
6. Preserve per-file licensing. A project's credit list is a discovery index,
   not a blanket license to copy its code.

## 6. Remaining searches

- Compare Resonance -> DolphinPS4 -> Crema -> M33 -> HENkaku -> Conker history
  for changes to allocator, thread wrappers, signal handlers, module loading,
  DNS, crash logging, and VideoOut WSI.
- Inspect G-Diffuser-PS4's complete build and source tree, especially its
  WSI and pipeline cache.
- Audit xemu-ps4, SSBU, BOTW, TOTK-PS4, OpenCE-PS4, SRB2-PS4, PS4Liberty,
  ConkerLR-PS4 and Godot's PS4 branch, plus all projects
  named by the developer's public credits before declaring the ecosystem map
  complete.
- Find the exact RPCS3 PS4 FFmpeg build and compare its headers and archive
  manifests with WinePS4's FFmpeg 7.1.1 image-only build.
- Identify the public/private CEMUPS4 source or build artifact lineage used
  by PS4SX2; do not assume it is interchangeable with ps4-kit.
