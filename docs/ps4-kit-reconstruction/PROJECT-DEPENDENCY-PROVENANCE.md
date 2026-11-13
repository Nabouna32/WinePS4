# External dependency and developer provenance map

Date: 2026-11-13

This follows the credits of the PS4 projects that name ps4-kit and then follows
their upstream dependencies. It is a build-provenance map, not a claim that
every project belongs inside the shared kit. Project engines, recompilers,
host tools and game-specific codecs remain project-owned.

## 1. Shared PS4 platform/toolchain stack

| Component | Repository / upstream | Credited maintainer | Contribution |
|---|---|---|---|
| OpenOrbis PS4 Toolchain | [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) | OpenOrbis contributors | PS4 target compiler integration, headers, libc/system imports, linker scripts, create-fself |
| PacBrew | [PacBrew](https://github.com/PacBrew), [ps4-openorbis](https://github.com/PacBrew/ps4-openorbis), [portlibs](https://github.com/PacBrew/ps4-openorbis-portlibs) | PacBrew contributors | Distribution of OpenOrbis and ported libraries/utilities |
| LLVM | [llvm-project](https://github.com/llvm/llvm-project) | LLVM contributors | clang, lld, libc++, libc++abi, libunwind |
| Mesa RADV | [Mesa](https://gitlab.freedesktop.org/mesa/mesa) | Mesa contributors | Vulkan driver and AMD compiler; PS4 GNM winsys is a port |
| dlmalloc | [Doug Lea's dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) | Doug Lea | mspace allocator engine; downstream PS4 adapter has separate licensing |
| LÖVE PS4 baseline | [Mari0/love-ps4](https://github.com/Mari0/love-ps4), [iHaiDeeZ/love-ps4](https://github.com/iHaiDeeZ/love-ps4) | LÖVE Development Team / PS4 port maintainers | First working heap, linker wrappers, thread destructors, Piglet/module setup |
| PSChrome malloc lineage | Downstream snapshots in [SSBU](https://github.com/iHaiDeeZ/SSBU) and [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | Credited as PSChrome in SSBU | Early malloc replacement, direct-memory query/allocation, random source and CRT replacement research; standalone public repo not found |
| Runtime/WSI lineage | [Resonance](https://github.com/iHaiDeeZ/Resonance), [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4), [M33](https://github.com/iHaiDeeZ/M33), [Crema](https://github.com/iHaiDeeZ/Crema), [SMW](https://github.com/iHaiDeeZ/SMW) | ShiroKlein / iHaiDeeZ | Heap, threads, diagnostics, profiler, networking, VideoOut WSI, RADV/GNM, packaging |

Modern ports use clang/libc++ 21; older PacBrew clang 12 packages are a
different generation. Keep host compilers, target sysroot and package tools
separate and pin each version.

## 2. Direct consumers and their project-owned dependencies

### WinePS4

- [WinePS4](https://github.com/iHaiDeeZ/WinePS4), by iHaiDeeZ / ShiroKlein.
- [Wine](https://www.winehq.org), Wine project, 11.0: Windows compatibility layer.
- [DXVK](https://github.com/doitsujin/dxvk), Philip Rebohle and contributors, 3.1.1: Direct3D 8–11 over Vulkan.
- [FreeType](https://freetype.org), 2.13.3: fonts.
- [FFmpeg](https://ffmpeg.org), 7.1.1: image decoders only in the desktop picture path.
- [stb](https://github.com/nothings/stb), Sean Barrett and contributors: image/font helpers.
- Platform requirement: private ps4-kit revision 56b3d86, with Wine-specific window-desktop WSI and process-role heap policy.

Wine's image FFmpeg recipe is not the same as M33's FFmpeg 7.1 sysroot or
the XMA/WMA decoder subset in ReXGlue projects.

### MGR-PS4

- [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4), by iHaiDeeZ / ShiroKlein.
- [mgr-nx-deko](https://github.com/tuancapoo/mgr-nx-deko), by tuancapoo: game port/recompiler project.
- Its credits name sa2-nx by ChanseyIsTheBest, masseffect-nx by NebadasSwifty, [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND, and NFSMW Recompiled by madelrandel-blip in its ancestry.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), v0.10.0, by Tom Clay (tomcl7) and contributors; portions derive from [Xenia](https://xenia.jp) by Ben Vanik and contributors.
- [Renut](https://github.com/masterspike52/reNut), by masterspike52, with Banjo-NX/SA2-NX adaptations: renderer. The upstream Renut repository has no general licence file; SA2-NX documents a project-specific permission. Do not copy its code into the kit without resolving the applicable permission.
- [plume](https://github.com/renderbag/plume), by renderbag: Vulkan backend (MIT).
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), hedge-dev and contributors: host-side shader/code-generation tool (MIT).
- [dxc-bin](https://github.com/renderbag/dxc-bin), renderbag: host-only DXC binaries.
- FFmpeg XMA/WMA decoder subset from ReXGlue's FFmpeg submodule, configured for PS4.
- Platform requirement: private kit 79fba0f; ReXGlue remains project-owned.

### MP3-PS4

- [MP3-PS4](https://github.com/iHaiDeeZ/MP3-PS4), by iHaiDeeZ / ShiroKlein.
- [mp3-nx](https://github.com/VasilkovND/mp3-nx), by VasilkovND: game port/code generation. Its credits name gta4-nx, ds2-nx, gears2-nx, dantes-inferno-nx, [NFSMW-NX](https://github.com/StevensND/nfsmw-nx), [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp) and [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid) in its ancestry.
- ReXGlue SDK v0.10.0 by Tom Clay; the MP3 tree also references [rexglue-sdk-nx](https://github.com/nx-mod/rexglue-sdk-nx) by nx-mod and portions from Xenia.
- Reuses the MGR port's Renut, plume, XenosRecomp and DXC path; MP3 adds its own renderer hooks, HDR, tiling and resolve changes.
- FFmpeg XMA/WMA decoder subset is project-specific.
- Platform requirement: private kit 56b3d86; ReXGlue and host generators remain project-owned.

### TTT2-PS4

- [TTT2-PS4](https://github.com/iHaiDeeZ/TTT2-PS4), by iHaiDeeZ / ShiroKlein.
- [tekken-tag-tournament-2-pc](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc), by shihabal3amri: game recompilation.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), hedge-dev and contributors: PowerPC to C++ host recompiler.
- [DXVK](https://github.com/doitsujin/dxvk), Philip Rebohle and contributors, 3.1.1.
- [vkd3d-shader](https://gitlab.winehq.org/wine/vkd3d), Wine project, 2.1.
- FFmpeg n7.1: only XMA/WMA Pro decoder paths, LGPL-2.1-or-later.
- Platform requirement: private kit ff30ff6; XenonRecomp and game runtime stay project-owned.

### ConkerLR-PS4

- [ConkerLR-PS4](https://github.com/iHaiDeeZ/ConkerLR-PS4), by iHaiDeeZ / ShiroKlein.
- [conker-lr-recomp](https://github.com/rashevskyv/conker-lr-recomp), by rashevskyv: original Xbox recomp.
- Its credits identify flatout2-sw by antoxa2584x and xboxrecomp by sp00nz as upstream layers.
- The patch also adds xemu MCPX APU code, AMD FSR 1 and volk by Arseny Kapoulkine, each with its own notice.
- Platform requirement: private kit 87a517c; its build script has a libs mode that builds PS4 runtime libraries with stand-in generated functions.

### Godot PS4 branch

- [Godot Engine](https://github.com/godotengine/godot), PS4 branch maintained in [iHaiDeeZ/godot](https://github.com/iHaiDeeZ/godot/tree/ps4).
- Godot's SCons build produces the PS4 engine libraries; a separate CMake eboot project includes ps4-kit.cmake, links RADV and calls ps4_kit_app().
- Platform requirement: private kit; public README/build script does not pin a commit.

## 3. Shared runtime reference ports (not all private-kit consumers)

| Project | Developer/upstream credits | What is useful |
|---|---|---|
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | ShiroKlein; Dolphin emulator team; Mesa, OpenOrbis, PacBrew, LLVM; dlmalloc by Doug Lea | Modern clang 21/libc++ 21 toolchain, mspace allocator adapter, CRT replacement table, GNM winsys, native VideoOut |
| [Resonance](https://github.com/iHaiDeeZ/Resonance) | ShiroKlein; [Flycast](https://github.com/flyinghead/flycast) team; Mesa, OpenOrbis, PacBrew | Earlier runtime, heap, WSI, SMB/NFS integrations and allocator evolution |
| [M33](https://github.com/iHaiDeeZ/M33) | ShiroKlein; [PPSSPP](https://github.com/hrydgard/ppsspp) team; RPCS3 PS4 FFmpeg artifacts | Direct-memory PSP arena, VideoOut WSI, FFmpeg 7.1 sysroot provenance |
| [Crema](https://github.com/iHaiDeeZ/Crema) | ShiroKlein; [Cemu](https://github.com/cemu-project/Cemu) project; Mesa, DolphinPS4 | GNM winsys, write-combined cache coherency, runtime |
| [HENkaku-PS4](https://github.com/iHaiDeeZ/HENkaku-PS4) | ShiroKlein; [Vita3K](https://github.com/Vita3K/Vita3K) team, [Vita3K+](https://github.com/nckstwrt/Vita3K), dynarmic | Later integration of runtime, DNS, diagnostics, sysroot and VideoOut |
| [Conker-PS4](https://github.com/iHaiDeeZ/Conker-PS4) | ShiroKlein; [CBFD-Recompiled](https://github.com/sciaschi/CBFD-Recompiled), [N64Recomp](https://github.com/N64Recomp/N64Recomp), RT64/N64ModernRuntime | HENkaku runtime lineage, scheduler/heap-lock lessons, WSI |
| [G-Diffuser-PS4](https://github.com/iHaiDeeZ/G-Diffuser-PS4) | ShiroKlein; G-Diffuser upstream | DolphinPS4 toolchain, WSI and pipeline-cache reference |
| [xemu-ps4](https://github.com/iHaiDeeZ/xemu-ps4) | ShiroKlein; [xemu](https://github.com/xemu-project/xemu), QEMU | Uses Resonance/DolphinPS4/Crema build lineage; native VideoOut frontend |
| [SSBU](https://github.com/iHaiDeeZ/SSBU) | ShiroKlein; [WindowSmash](https://github.com/SNO2207/WindowSmash) by SNO2207; DXVK; vkd3d-shader; Mesa | Public VideoOut WSI, system libc malloc replacement, PSChrome snapshots |
| [SMW](https://github.com/iHaiDeeZ/SMW) | ShiroKlein; [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) by BelmanteGu; Ryujinx/Ryubing | Recompiler and PS4 runtime/VideoOut integration; published SDK bundle |
| [BOTW](https://github.com/iHaiDeeZ/BOTW) / [TOTK-PS4](https://github.com/iHaiDeeZ/TOTK-PS4) | ShiroKlein; BelmanteGu; [Ryujinx](https://github.com/Ryujinx/Ryujinx)/[Ryubing](https://codeberg.org/Software_arch/Ryubing); [MacNativeGames](https://github.com/ly05010419/MacNativeGames); [zeldaret/botw](https://github.com/zeldaret/botw) | Recompiler history, sysroot and shader tools. SuperMarioWonderRecomp's original GitHub repo was reported gone; BOTW retains history in its recomp branch |
| [OpenCE-PS4](https://github.com/iHaiDeeZ/OpenCE-PS4) | ShiroKlein; [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) contributors; Mesa Zink/RADV | 32-bit guest and Zink/RADV integration, public runtime lineage |
| [PS4Liberty](https://github.com/iHaiDeeZ/PS4Liberty) | ShiroKlein; [LibertyRecomp](https://github.com/OZORDI/LibertyRecomp) by Prince Ozordi; [MarathonRecomp](https://github.com/sonicnext-dev/MarathonRecomp); ReXGlue/Xenia | Discontinued independent PS4 runtime: 512 MiB flexible heap, direct-memory overflow, 4.5 GiB guest mapping |
| [Shattered Pixel Dungeon PS4](https://github.com/iHaiDeeZ/shattered-pixel-dungeon-ps4) | ShiroKlein; Evan Debenham; libGDX; PacBrew SDL2; DolphinPS4 runtime | Another downstream consumer of DolphinPS4 dlmalloc/runtime and love-ps4 SDL patch |
| [Mari0-PS4](https://github.com/iHaiDeeZ/mari0-ps4) | ShiroKlein; Mari0 by Maurice Guégan; LÖVE | Consumer of LÖVE for PS4 engine/runtime |
| [SRB2-PS4](https://github.com/iHaiDeeZ/SRB2-PS4) | ShiroKlein; [STJr/SRB2](https://github.com/STJr/SRB2); DeCast-S4 / love-ps4 | Legacy SDL2/VideoOut runtime reference; standalone DeCast-S4 source repo not located |
| [PS4SX2](https://github.com/iHaiDeeZ/PS4SX2) | ShiroKlein; [PCSX2](https://github.com/PCSX2/pcsx2); [PS5SX2](https://github.com/iHaiDeeZ/PS5SX2) | Separate CEMUPS4 base, not established as a ps4-kit consumer |

## 4. Recompiler / SDK repositories that must stay outside ps4-kit

These projects are required to build their own applications, but are not
generic platform runtime dependencies:

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), Tom Clay and contributors, with Xenia-derived portions.
- [rexglue-sdk-nx](https://github.com/nx-mod/rexglue-sdk-nx), nx-mod's Switch-derived ReXGlue fork.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), hedge-dev and contributors.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), hedge-dev and contributors.
- [WindowSmash](https://github.com/SNO2207/WindowSmash), SNO2207.
- [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp), BelmanteGu (original repo unavailable in current GitHub lookup; history is preserved in BOTW's recomp branch).
- [N64Recomp](https://github.com/N64Recomp/N64Recomp), N64Recomp contributors.
- [CBFD-Recompiled](https://github.com/sciaschi/CBFD-Recompiled), sciaschi.
- [xemu](https://github.com/xemu-project/xemu) / QEMU.
- [Vita3K](https://github.com/Vita3K/Vita3K), Vita3K contributors, and [Vita3K+](https://github.com/nckstwrt/Vita3K).
- [Cemu](https://github.com/cemu-project/Cemu), Exzap, Petergov and contributors.
- [PPSSPP](https://github.com/hrydgard/ppsspp), PPSSPP contributors.
- [Flycast](https://github.com/flyinghead/flycast), flyinghead and contributors.

Keep host-side code generators, shader compilers and game-specific runtimes
built in each consumer's own host/target build graph. ps4-kit should expose
the target ABI, common runtime, RADV/WSI profiles and packaging boundary only.

## 5. Unresolved upstream leads

- PSChrome: downstream code and credit attribution exist; no standalone public
  repository was identified. Preserve the attribution and do not invent a URL.
- DeCast-S4: named in SRB2-PS4's runtime attribution alongside love-ps4;
  no standalone public repository identified in this pass.
- CEMUPS4: PS4SX2 scripts refer to $HOME/cemups4 and $HOME/cemups4-build;
  public source/artifact lineage is not yet identified.
- RPCS3 PS4 FFmpeg build: M33 documents FFmpeg 7.1 artifacts from the RPCS3
  PS4 build tree; exact source commit, patches, configure flags and archive
  manifest still need recovery.
- Renut: MGR/MP3 credit masterspike52's renderer but say its upstream repo has
  no general licence file; no code copying until permissions are resolved.
- SuperMarioWonderRecomp: original public GitHub repository is unavailable in
  current lookup; BOTW preserves a recomp branch with the source history used
  for its port.

## 6. Licensing rules for reconstruction

1. A credit line identifies provenance; it is not permission to copy code.
2. Keep the license/notice attached to each vendored file and preserve
   subcomponent notices.
3. Distinguish dlmalloc's upstream MIT-0/CC0 license from downstream PS4
   adapters and runtime code.
4. Do not assume Mesa's top-level license applies identically to every winsys
   or generated patch; retain file-level notices.
5. Do not absorb GPL/LGPL project-owned recompiler code, game-specific runtime,
   Sony files, game assets, or host binaries into the shared kit.


## 7. Independent downstream build cross-check: SeriousSamPS4-RADV

[JarnoVos/SeriousSamPS4-RADV](https://github.com/JarnoVos/SeriousSamPS4-RADV)
is not a private-kit consumer, but it is a useful independent integration of
DolphinPS4's published RADV stack. Its README and THIRD-PARTY.md state:

- Mesa 26.0.8, DolphinPS4's mesa-ps4.patch and GNM winsys;
- clang 21 for compilation, but PacBrew LLD 12.0.1 for linking because the
  LLVM 21 linker did not work in that build;
- OpenOrbis SDL-PS4 pinned at 7774cf3 (zlib);
- ps4-libjbc pinned at 835fe016 for jailbreak mount support;
- dlmalloc 2.8.6, MIT-0, for the retail malloc replacement;
- its own PS4 platform integration, based on Nichlas Ek's older
  [SeriousSamPS4](https://github.com/NichlasEk/SeriousSamPS4), but using RADV
  instead of that project's OpenGNM driver.

This gives us an additional real downstream warning: do not assume clang and
LLD must come from the same LLVM release. Keep compiler and linker versions
independently configurable in the reconstructed environment. This is a
project-specific known-good pairing, not a universal requirement for all kit
consumers.

References:
- [SeriousSamPS4-RADV README](https://github.com/JarnoVos/SeriousSamPS4-RADV)
- [THIRD-PARTY.md](https://github.com/JarnoVos/SeriousSamPS4-RADV/blob/main/THIRD-PARTY.md)
- [OpenOrbis SDL-PS4](https://github.com/OpenOrbis/SDL-PS4/tree/7774cf3)
- [ps4-libjbc](https://github.com/bucanero/ps4-libjbc/tree/835fe016)
- [DolphinPS4 Mesa build guide](https://github.com/iHaiDeeZ/DolphinPS4/blob/main/BUILDING.md)

The README says the project is GPL-2.0 because it is a port of Serious Engine;
its own platform integration must therefore not be copied wholesale into
ps4-kit. Only its public toolchain and dependency facts are used here.
