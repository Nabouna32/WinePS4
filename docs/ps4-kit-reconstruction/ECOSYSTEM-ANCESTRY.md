# PS4 ecosystem ancestry and source map

Date: 2026-11-13

This note widens the search beyond the author of WinePS4. It separates (a) sources the WinePS4 credits explicitly identify as inputs to the private ps4-kit, (b) other public projects that expose reusable PS4 build infrastructure, and (c) older or independent SDK lineages. Similarity is not treated as proof of code copying.

## 1. The strongest provenance statement is in WinePS4's own credits

WinePS4's public `CREDITS.md` describes ps4-kit as the shared PS4 base by ShiroKlein and says it was **merged from**:

- [Resonance](https://github.com/iHaiDeeZ/Resonance)
- [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)
- [M33](https://github.com/iHaiDeeZ/M33)
- [Crema](https://github.com/iHaiDeeZ/Crema)
- and other ports.

It names the kit's responsibilities as runtime, heap, threads, RADV port/window system and packaging. This is direct provenance from the project itself, not an inference from similar names. The unpublished commit `56b3d86` remains the build's pin, but the full source tree and exact merge map are not public in WinePS4.

**Consequence:** these four projects are the primary reconstruction corpus. Compare their history and files component by component; don't treat ps4-kit as a single monolithic SDK that must be recreated from scratch.

## 2. The author's earlier public PS4 port: love-ps4

[iHaiDeeZ/love-ps4](https://github.com/iHaiDeeZ/love-ps4) is an earlier public port by the same author. Its README says it is LÖVE 11.4 for PS4, built with OpenOrbis and PacBrew, and credits those toolchains. This is useful as a historical baseline for the author's first PS4 build conventions, but it predates the newer Clang 21 / libc++ 21 / Mesa RADV stack used by the later ports.

Use it to trace older packaging, controller, display, and OpenOrbis/PacBrew integration patterns. Do not assume its graphics path or old dependencies are ABI-compatible with WinePS4.

## 3. Public independent PS4 SDK/porting ecosystems

### OpenOrbis + PacBrew

- [OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) supplies the public PS4 toolchain sources, headers, library stubs and build utilities. Its README explicitly recommends release archives/installers to avoid rebuilding its libraries and binaries.
- [PacBrew/ps4-openorbis](https://github.com/PacBrew/ps4-openorbis) and [ps4-openorbis-portlibs](https://github.com/PacBrew/ps4-openorbis-portlibs) package the OpenOrbis base and third-party portlibs. This is the practical bootstrap and recipe catalogue already audited in DEPENDENCY-RECOVERY.md.

This is the base layer acknowledged by WinePS4. The PacBrew compiler package is LLVM 12.0.1; that must not be confused with the host LLVM 21 tools used in the modern port build scripts.

### orbis-ports: a separate platform layer and RADV implementation

The [orbis-ports organisation](https://github.com/orbis-ports) publishes a separate, well-documented stack:
- [orbis-compat](https://github.com/orbis-ports/orbis-compat): OpenOrbis compatibility overlay, libc/pthread shims, Vulkan loader shim, logging and packaging-related platform fixes.
- [orbis-porting-kit](https://github.com/orbis-ports/orbis-porting-kit): CMake toolchain, packaging/deploy scripts, Vulkan loader shim and examples. Its README records that it was assembled around the overlay and OpenOrbis, and its release bundles the SDK, overlay and Mesa/RADV.
- [mesa-ps4](https://github.com/orbis-ports/mesa-ps4) and [ps4-mesa-docs](https://github.com/orbis-ports/ps4-mesa-docs): independent public PS4 RADV/GNM port and its engineering record.

These are valuable independent references for OpenOrbis header/libc mismatches, linker scripts, RADV's ICD boundary, and VideoOut presentation. But their WSI is not automatically WinePS4's window-mode WSI: Wine's public API explicitly sends completed window frames through a callback, whereas a headless/native VideoOut surface is a different presentation contract. Reuse lower-level Mesa or platform fixes only after matching the exact interface and revision.

The orbis-porting-kit repository says it has been superseded by orbsdk. Treat it as a historical, pinned reference, not a moving dependency.

### PS4MC: independent dependency recipes, unusually close version evidence

[SoftwareRat/ps4mc](https://github.com/SoftwareRat/ps4mc) is a separate Kodi-on-PS4 effort. Its public `ps4/scripts/10-build-deps.sh` says its recipes follow PacBrew's `ps4-openorbis-portlibs` PKGBUILDs, driven directly with its own `ps4-kodi.cmake` flags. It builds:
- FreeType **2.13.3** static, the exact FreeType version WinePS4 documents.
- FFmpeg **7.1** static, close to but not the exact WinePS4 **7.1.1** source version.
- zlib, bzip2, libpng, fribidi, harfbuzz, libass, OpenSSL, curl and many other libraries.

The PS4MC toolchain uses OpenOrbis, the FreeBSD 12 ELF target, and a separate Clang 18 host compiler plus its own compatibility header shims. It is therefore a valuable independent recipe and flags reference, especially for FreeType 2.13.3, but it is **not** a drop-in sysroot or proof of binary compatibility with ps4-kit. Its FFmpeg profile enables network/audio/subtitle-related components and is not Wine's minimal image-only configuration.

### Older orbisdev / PS4SDK lineage

- [orbisdev/orbisdev](https://github.com/orbisdev/orbisdev) is an older meta-build which assembles orbisdev-toolchain, sdkvanilla, liborbis, raylib, LIEF, pkgtool and gp4gen.
- [orbisdev/PS4SDK](https://github.com/orbisdev/PS4SDK) describes a modular open-source SDK with userland and kernel support, based on a FreeBSD 9.0 lineage and early firmware targets.

These are useful historical references for PS4 formats, stubs, packaging and the earlier homebrew ecosystem. They are not the same SDK architecture as OpenOrbis/PacBrew and are not candidates to replace the modern OpenOrbis sysroot without a deliberate ABI port.

## 4. What the cross-project evidence says about ps4-kit

| ps4-kit area | Best public evidence to mine | What can likely be reused |
|---|---|---|
| OpenOrbis toolchain / packaging | OpenOrbis + PacBrew; love-ps4 | Existing tool binaries, headers, package recipes and conventions |
| Heap, threads, runtime, process startup | Resonance, DolphinPS4, M33, Crema histories (the explicitly named merge sources) | Public implementations and their commit ancestry, after API/ABI comparison |
| RADV / Mesa archives | Crema, DolphinPS4, SMW SDK archive; independently orbis-ports/mesa-ps4 | Source patches, build recipes and candidate static archives |
| Native VideoOut WSI | Crema / DolphinPS4 / orbis-ports Mesa | Low-level VideoOut and RADV pieces only |
| Wine window WSI and Vulkan callback | WinePS4's public declarations and kit-facing call sites | Reconstruct the exact public callback contract; no equivalent is established yet |
| FreeType 2.13.3 | WinePS4 recipe requirements and PS4MC's independent recipe | Reuse upstream source and compare target flags |
| FFmpeg 7.1.1 image subset | WinePS4's own configure script; RPCS3 path; PS4MC FFmpeg 7.1 recipe as comparison | Reuse matching source/wrappers; retain Wine's image-only configure profile |

## 5. Search priorities from here

1. Diff the runtime/heap/thread/packaging files and commit histories in Resonance, DolphinPS4, M33 and Crema. This is the highest-confidence route because WinePS4 explicitly names them as merge sources.
2. Follow the older love-ps4 history backwards to understand the author's original OpenOrbis/PacBrew build setup and what changed in later ports.
3. Compare the SMW SDK archive and its 16 Mesa archive closure with Crema/DolphinPS4 and the private kit's documented link contract. Treat the SDK as a candidate artifact, not an ABI match until linked.
4. Use PS4MC's FreeType 2.13.3 recipe as an independent cross-check; compare every flag with the exact WinePS4 build requirements before reusing it.
5. Mine orbis-ports for independent validation of the PS4 libc, linker, RADV and VideoOut issues, but preserve Wine's window-mode callback/WSI semantics.

## 6. Scope and safety

All repositories and build scripts above are public research sources. This reconstruction must remain an independent, documented implementation of public interfaces. Do not import proprietary Sony SDK files, private credentials, or binaries of unknown provenance. Record source URL, commit, path and licence for every reused file. Keep all changes on `research/ps4-kit-reconstruction`; do not merge the PR or modify `main`, and do not change WinePS4's `app/`, `port/`, `shell/` or `patches/` behavior.
