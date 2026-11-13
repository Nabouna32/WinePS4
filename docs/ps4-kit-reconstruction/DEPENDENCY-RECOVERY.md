# Dependency recovery map (public sources first)

This document corrects the initial reconstruction strategy: reuse existing PS4 packages,
build trees and project recipes wherever possible. Reconstruct only the missing glue or
a dependency for which no compatible public artifact exists.

## Findings at a glance

| Component | Existing source found | Version / status | Decision for WinePS4 |
|---|---|---|---|
| OpenOrbis toolchain distribution | [PacBrew/ps4-openorbis](https://github.com/PacBrew/ps4-openorbis), [PacBrew/pacbrew-packages](https://github.com/PacBrew/pacbrew-packages) | Packaged OpenOrbis toolchain | Install/reuse; do not rebuild toolchain from source |
| PS4 portlibs | [PacBrew/ps4-openorbis-portlibs](https://github.com/PacBrew/ps4-openorbis-portlibs) | Includes FreeType 2.10.1 and FFmpeg 5.0 recipes | Useful package fallback, but not the exact versions WinePS4 documents |
| LLVM / libc++ | [DolphinPS4 toolchain scripts](https://github.com/iHaiDeeZ/DolphinPS4/tree/main/toolchain), also used by Resonance/M33 | Clang 21, libc++ 21; libc++ source pinned to LLVM 21.1.6 | Reuse public build scripts only if a matching installed toolchain/sysroot is unavailable |
| Mesa RADV/GNM | [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4), [Crema](https://github.com/iHaiDeeZ/Crema), [Resonance](https://github.com/iHaiDeeZ/Resonance) | Mesa 26.0.8 PS4 port | Already tracked by the Mesa kit integration; reuse public source/patch |
| FreeType | [WinePS4 BUILDING.md](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/docs/BUILDING.md); [PS4MC recipe](https://github.com/SoftwareRat/ps4mc/blob/1a6ef9fc00836a34ec0091f26324695f58beb0dd/ps4/scripts/10-build-deps.sh) | Wine requires 2.13.3 static; PacBrew portlibs recipe is 2.10.1 | Reuse the exact 2.13.3 recipe and PS4 toolchain; no need to invent a port |
| FFmpeg for Wine desktop images | [WinePS4 build script](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/scripts/build-ffmpeg-img.sh) | FFmpeg 7.1.1, static, image-only codecs; source expected at `~/rpcs3-build/src/deps/ffmpeg` | Reuse the existing WinePS4 recipe and exact source version; don't substitute PacBrew FFmpeg 5.0 |
| FFmpeg in M33 | [M33 technical notes](https://github.com/iHaiDeeZ/M33/blob/78f0f40edd55b6183631150cfe609170b5c530b8/docs/TECHNICAL.md), [seed script](https://github.com/iHaiDeeZ/M33/blob/78f0f40edd55b6183631150cfe609170b5c530b8/scripts/seed-build-tree.sh) | FFmpeg 7.1.1 copied from RPCS3's PS4 sysroot | Confirms an existing built artifact/source path; investigate access to the matching RPCS3 source/toolchain tree |
| General Kodi dependency set | [PS4MC dependency builder](https://github.com/SoftwareRat/ps4mc/blob/1a6ef9fc00836a34ec0091f26324695f58beb0dd/ps4/scripts/10-build-deps.sh) | FFmpeg 7.1 plus FreeType 2.13.3, zlib, bzip2, libpng, fribidi, harfbuzz, libass, curl, OpenSSL, etc. | Strong reference for future native Kodi work; not a drop-in Wine dependency bundle |

## PacBrew: what is actually available

The PacBrew package catalogue lists two PS4 package families:
- `ps4-toolchain/*`: clang, headers, musl, libc++, create-fself, create-gp4, readoelf,
  pkgtools, pkg-config and environment variables.
- `ps4-portlibs/*`: zlib, bzip2, libpng, libjpeg-turbo, libwebp, FreeType, Mbed TLS,
  curl, audio libraries, SDL2, libass, FFmpeg, libmpv, glm, tinyxml2, fmt, libvpx,
  libarchive and more.

Repository install instructions use the PacBrew pacman repository and:
```sh
sudo pacman -S ps4-openorbis ps4-openorbis-portlibs
```

Important version distinctions from the actual PKGBUILDs:
- PacBrew's `ps4-openorbis-clang` package is LLVM/Clang 12.0.1, while the modern DolphinPS4,
  M33 and WinePS4 build recipes use LLVM 21 tools. Install/reuse PacBrew's OpenOrbis headers,
  musl, SDK utilities and package tools, but use a separate host LLVM 21 toolchain for this
  reconstruction; do not let the old bundled Clang 12 shadow it.
- PacBrew FreeType is 2.10.1; WinePS4 documents 2.13.3.
- PacBrew FFmpeg is 5.0; WinePS4 documents 7.1.1 and M33 uses 7.1.1.
So PacBrew is a valuable source for the OpenOrbis base and many transitive libraries,
but those older compiler/FreeType/FFmpeg versions must not silently replace the versions
expected by the current WinePS4 build.

## The credits trail and reusable build trees

The credits/technical docs form a dependency chain rather than isolated acknowledgements:

1. DolphinPS4 credits OpenOrbis, PacBrew, LLVM 21 and love-ps4; its BUILDING guide documents
   the OpenOrbis install path `/opt/pacbrew/ps4/openorbis`, Clang 21, Mesa 26.0.8,
   libc++ 21 and host Meson/glslang tools.
2. Resonance says its build tree is seeded from Crema's existing Cemu build tree
   (`~/cemups4-build`), copying the sysroot, host compatibility directory and Mesa build.
3. M33 says its tree is seeded from Resonance's build tree plus RPCS3's FFmpeg 7.1.1
   sysroot artifacts. Its script copies static FFmpeg archives and headers rather than
   rebuilding them.
4. HENkaku credits explicitly say its PS4 sysroot libraries (including FFmpeg 7.1) come
   from M33's build, and its PS4 runtime/VideoOut pieces come from Resonance, DolphinPS4,
   M33 and Crema.

This is strong evidence that the original author reuses a shared, layered build workspace.
We should follow that pattern, not rebuild all of those pieces in WinePS4.

## Exact WinePS4 dependency requirements

The upstream WinePS4 BUILDING guide says:
- FreeType 2.13.3, static, installed under `~/wineps4-build/freetype`.
- FFmpeg image decoders installed under `~/wineps4-build/ffmpeg-img`.
- The existing `scripts/build-ffmpeg-img.sh` fetches neither source nor toolchain: it expects
  FFmpeg at `~/rpcs3-build/src/deps/ffmpeg` and compiler wrappers at `~/rpcs3/toolchain/bin`.
- Its configure flags enable only the needed image decoders/demuxers/parsers, disable network,
  programs and shared libraries, and omit zlib because Wine's stb path already handles PNG.

The local `rpcs3` checkout and its PS4 wrapper directory are referenced by WinePS4, M33 and TOTK.
The original public repository URL for that RPCS3 workspace is still unlocated, but TOTK-PS4 and
DolphinPS4 publish matching Clang 21/OpenOrbis `env.sh`, `ps4-cc` and `ps4-c++` wrappers that
can be reused rather than recreated. TOTK's [FFmpeg VP9 build script](https://github.com/iHaiDeeZ/TOTK-PS4/blob/aa5ad9b01510f3e6cdf6f6c44408bc3a645727ac/scripts/build-ffmpeg-vp9.sh)
independently confirms the shared `~/rpcs3-build/src/deps/ffmpeg` source path and that RPCS3's
`scripts/build-deps.sh ffmpeg` fetches FFmpeg 7.1.1. The remaining provenance/access question is
the RPCS3 recipe/revision and its PS4-specific FFmpeg patches/configuration, not the wrappers.

## What to do next

1. Recover/install PacBrew's OpenOrbis distribution and the LLVM 21 host tools (the user has
   not installed them yet, so no target build can run locally at this point).
2. Inventory whether an existing Crema/Resonance/DolphinPS4/M33 sysroot or RPCS3 PS4 build tree
   is already available. If so, reuse it read-only and copy only the required artifacts.
3. Build FreeType 2.13.3 from its existing upstream source using the public PS4 dependency recipe
   and install it at the path the unchanged WinePS4 CMake files expect.
4. Reuse WinePS4's own FFmpeg 7.1.1 image-only build script once the matching RPCS3 source and
   wrapper paths are recovered; otherwise adapt the public PS4MC FFmpeg 7.1 recipe only after
   comparing its configure options and source version.
5. Keep the existing Mesa 26.0.8 patch/apply check and RADV build path. Do not replace Wine's
   window WSI with the native VideoOut WSI.
6. Only after dependencies are present, run the unchanged `app/build.sh all`, then
   `app/package.sh`.

No source under `app/`, `port/`, `shell/` or `patches/` is to be modified for this work.


## PacBrew source-level audit (2026-11-13)

The package catalogue is not just a list of package names: its submodules pin the exact source trees used by PacBrew. At the current `PacBrew/pacbrew-packages` master revision, the gitlinks are:

- `ps4-toolchain` → `PacBrew/ps4-openorbis`, commit `16f27e38760ea21eaf1187d0edbf4342c4a44021`.
- `ps4-portlibs` → `PacBrew/ps4-openorbis-portlibs`, commit `d6a523a5a4a7b86846d91134d40a34357721a273`.

Sources: [package catalogue](https://github.com/PacBrew/pacbrew-packages), [submodule pins](https://github.com/PacBrew/pacbrew-packages/blob/master/.gitmodules), [PS4 package list](https://github.com/PacBrew/pacbrew-packages/blob/master/pacbrew-packages.cfg).

### Exact reusable build recipes

- [PacBrew FFmpeg PKGBUILD](https://github.com/PacBrew/ps4-openorbis-portlibs/blob/d6a523a5a4a7b86846d91134d40a34357721a273/ffmpeg/PKGBUILD): FFmpeg **5.0**, release tarball SHA-256 `51e919f7d205062c0fd4fae6243a84850391115104ccf1efc451733bc0ac7298`, static-only. It enables swscale/swresample, network and several protocols, and depends on zlib, bzip2, libass, libfribidi and FreeType. It is a real PS4 port recipe and useful reference for target flags/dependency structure, but **not** WinePS4's FFmpeg 7.1.1 image-only configuration.
- [PacBrew FreeType PKGBUILD](https://github.com/PacBrew/ps4-openorbis-portlibs/blob/d6a523a5a4a7b86846d91134d40a34357721a273/freetype/PKGBUILD): FreeType **2.10.1**, static, with zlib/bzip2/PNG. WinePS4 requires 2.13.3, so use as a build recipe reference, not as a version substitute.
- [PacBrew Clang PKGBUILD](https://github.com/PacBrew/ps4-openorbis/blob/16f27e38760ea21eaf1187d0edbf4342c4a44021/clang/PKGBUILD): bundled Clang/LLVM **12.0.1** with a PS4 patch. This is the historical PacBrew toolchain compiler; it is not the Clang 21 host toolchain used by the newer iHaiDeeZ ports.
- [PacBrew environment](https://github.com/PacBrew/ps4-openorbis/blob/16f27e38760ea21eaf1187d0edbf4342c4a44021/vars/ps4vars.sh): exports `OPENORBIS`, `OO_PS4_TOOLCHAIN`, `PS4SDK` at `/opt/pacbrew/ps4/openorbis`, compiler/binutils paths, pkg-config, target triple `x86_64-pc-freebsd12-elf`, PS4 defines, sysroot/include paths, and linker flags. This is the authoritative starting point for our environment compatibility layer.
- [PacBrew CMake toolchain](https://github.com/PacBrew/ps4-openorbis/blob/16f27e38760ea21eaf1187d0edbf4342c4a44021/vars/ps4.cmake): provides compiler selection, root-path lookup rules, CRT objects, standard libraries, direct-LLD link commands, `add_self()` and `add_pkg()`. This is particularly relevant to implementing the missing kit's app/package wrappers without guessing their lower-level mechanics.

The package catalogue also confirms that PacBrew has recipes for zlib, bzip2, libpng, libjpeg-turbo, libwebp, FreeType, Mbed TLS, curl, audio codecs, SDL2, libass, FFmpeg, libmpv, fmt, libvpx, libarchive and freegnm. These should be checked individually for version, flags and licence before selecting them for WinePS4.

### Important distinction: FFmpeg in WinePS4 versus PacBrew

WinePS4's own `scripts/build-ffmpeg-img.sh` is unusually specific: FFmpeg 7.1.1, static, image codecs only, no network, no programs/shared libraries, no PNG decoder (stb handles PNG), and a set of image demuxers/parsers. Its source path is `~/rpcs3-build/src/deps/ffmpeg` and its compiler wrappers are `~/rpcs3/toolchain/bin/ps4-cc` and `ps4-c++`. PacBrew's FFmpeg 5.0 recipe does not satisfy that contract and should not replace it.

MGR-PS4's credits independently show another FFmpeg lineage: FFmpeg comes from the ReXGlue SDK's `sdk/thirdparty/FFmpeg` submodule, with a PS4 configuration under `patches/ffmpeg/`. That build is for XMA/WMA game audio decoders, not WinePS4's desktop image decoding. It is evidence of a second reusable PS4 FFmpeg port/configuration, not a drop-in replacement. See [MGR-PS4 credits](https://github.com/iHaiDeeZ/MGR-PS4/blob/main/CREDITS.md).

### Practical reuse order

1. Reuse PacBrew's OpenOrbis tree, package tools, headers, musl/libc++ and `ps4vars.sh` conventions as the baseline; keep the Clang 12 versus Clang 21 choice explicit.
2. Reuse PacBrew portlib recipes only when their versions and configure options match the consumer. Preserve WinePS4's exact FreeType 2.13.3 and FFmpeg 7.1.1 requirements.
3. Recover the newer shared sysroot/build-tree lineage (Crema → Resonance/DolphinPS4 → M33 → HENkaku) and the RPCS3 PS4 wrappers before rebuilding those libraries.
4. Recreate only the private ps4-kit-specific glue that is not present in PacBrew/OpenOrbis or the public port repositories.

This audit is source-level only. It does not claim that packages were installed, binaries were retrieved from PacBrew's package server, or a target build was run.
