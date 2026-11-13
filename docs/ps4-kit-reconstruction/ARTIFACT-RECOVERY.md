# Public PS4 SDK and artifact recovery

Date: 2026-11-13

This is an evidence log for reusing already-built PS4 components. It does not claim that the
candidate artifacts have been downloaded, installed, or validated against WinePS4's private
ps4-kit build. The objective remains to compile the existing WinePS4 revision unchanged.

## 1. A real, downloadable sysroot + Mesa SDK exists

The author publishes **SMW PS4 SDK 1** as a GitHub release:

- Release: https://github.com/iHaiDeeZ/SMW/releases/tag/sdk-1
- Asset: https://github.com/iHaiDeeZ/SMW/releases/download/sdk-1/SMW-PS4-SDK-1.tar.xz
- Size: 14,741,280 bytes
- SHA-256: `15e7927cb01fa877c04ef59beca33907cb7fe3d39b035e3e296661c3a0f2e026`

The release description says it contains:
- `sysroot/`: libc++ 21 for OpenOrbis, the libc overlay, and the open-source libraries built into that sysroot.
- `build/mesa/`: static libraries from the Crema PS4 RADV/GNM build.

This is a concrete reusable artifact, not merely a recipe. It is small because the SDK contains the installed sysroot and the selected static Mesa archives, not the entire source/build trees.

### How the archive is made

The public [SMW `scripts/make-sdk.sh`](https://github.com/iHaiDeeZ/SMW/blob/main/scripts/make-sdk.sh) shows the exact contents-generation process:

1. Copy the existing `$PSB/sysroot`.
2. Remove documentation/man pages from the copy.
3. Read the Mesa archive list from `toolchain/ps4-love-style.cmake`.
4. Extract the members of each Mesa thin archive and write ordinary self-contained static archives into `build/mesa/`.
5. Write a `NOTICE.txt` naming the bundled projects and licences.
6. Package the result as `SMW-PS4-SDK-<version>.tar.xz`.

The SDK's Mesa archive list is the same 16-path closure currently recorded in this branch's `ps4-kit/mesa/archives.cmake` and used by DolphinPS4's `ps4_link_mesa()`. That is strong evidence that the archive can provide the candidate Mesa link inputs. It does **not** establish that the SDK's Mesa revision/ABI equals the unpublished WinePS4 kit pin `56b3d86`; we must validate the actual archives and final link before treating it as compatible.

Sources:
- https://github.com/iHaiDeeZ/SMW/blob/main/scripts/make-sdk.sh
- https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/toolchain/ps4-love-style.cmake
- https://github.com/Nabouna32/WinePS4/blob/research/ps4-kit-reconstruction/ps4-kit/mesa/archives.cmake

## 2. What the SDK does and does not replace

The generated `NOTICE.txt` in `make-sdk.sh` identifies the sysroot libraries as Boost, zlib, zstd, libzip, libpng, curl, OpenSSL, fmt, glm, glslang, pugixml, RapidJSON, libnfs and libsmb2, each under its own licence. The archive also contains the PS4 libc++ 21 runtime/headers and libc overlay.

It is **not a complete host toolchain**:
- The PacBrew/OpenOrbis toolchain itself is downloaded separately.
- Host LLVM/Clang 21 and its binutils are not the same thing as the target sysroot.
- The archive is not a Mesa source checkout; it carries selected built static archives.

It also does **not** claim to include WinePS4's two separately built libraries:
- FreeType 2.13.3 static at `~/wineps4-build/freetype`.
- FFmpeg 7.1.1 image-only static libraries at `~/wineps4-build/ffmpeg-img`.

Those exact paths are in WinePS4's public [build guide](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/docs/BUILDING.md) and its [FFmpeg image recipe](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/scripts/build-ffmpeg-img.sh).

## 3. PacBrew toolchain bootstrap is also scripted publicly

SMW publishes [`scripts/fetch-pacbrew.sh`](https://github.com/iHaiDeeZ/SMW/blob/main/scripts/fetch-pacbrew.sh). It reads PacBrew's package database, downloads every `ps4-openorbis-*` package, checks each package's SHA-256 against the database, and extracts them under `/opt/pacbrew` (or a supplied staging root). Its comment documents the package contents: Clang 12, LLD, create-fself, create-gp4, PkgTool, OpenOrbis headers and libc.

This gives us a reproducible public route to recover the OpenOrbis base without compiling LLVM/OpenOrbis from scratch. Keep the compiler distinction explicit: the PacBrew bundle's Clang 12 is not the Clang 21 host toolchain used by the newer DolphinPS4/M33-family build scripts.

Source: https://github.com/iHaiDeeZ/SMW/blob/main/scripts/fetch-pacbrew.sh

## 4. FFmpeg lineage: the useful artifact is in RPCS3's PS4 build tree

WinePS4's `scripts/build-ffmpeg-img.sh` records two exact local paths:

- Source tree: `$HOME/rpcs3-build/src/deps/ffmpeg`
- Compiler wrappers: `$HOME/rpcs3/toolchain/bin/ps4-cc` and `ps4-c++`

The script configures FFmpeg for the FreeBSD PS4 target, using LLVM 21 binutils, and builds a static image-only subset. It enables avcodec, avformat, avutil and swscale; disables swresample, networking, programs, shared libraries and autodetection; and selects image decoders/demuxers/parsers used by WinePS4's desktop. It deliberately leaves PNG decoding to stb.

M33's public [`scripts/seed-build-tree.sh`](https://github.com/iHaiDeeZ/M33/blob/78f0f40edd55b6183631150cfe609170b5c530b8/scripts/seed-build-tree.sh) confirms the artifact lineage:
- `RPC=$HOME/rpcs3-build`
- FFmpeg static archives copied from `$RPC/sysroot/lib/`: `libavcodec.a`, `libavformat.a`, `libavutil.a`, `libswresample.a`, `libswscale.a`.
- Corresponding `include/libavcodec`, `include/libavformat`, `include/libavutil`, `include/libswresample`, `include/libswscale` headers are copied.
- Matching pkg-config files are rewritten from the RPCS3 build root to the M33 build root.

M33's [technical guide](https://github.com/iHaiDeeZ/M33/blob/78f0f40edd55b6183631150cfe609170b5c530b8/docs/TECHNICAL.md) identifies this as FFmpeg 7.1 from the RPCS3 port's PS4 build. HENkaku's [credits](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/main/CREDITS.md) independently say its FFmpeg 7.1 sysroot libraries come from M33.

**Important distinction:** M33 copies a broader FFmpeg build for PSP video/audio (H.264, ATRAC3/3+, MP3, AAC). WinePS4 needs its own image-only configure profile. The source version and PS4 wrappers are the valuable shared inputs; do not blindly copy M33's configured libraries and assume they contain Wine's image codecs.

The exact public repository URL for the local `$HOME/rpcs3` wrapper checkout and the full `$HOME/rpcs3-build` source/build workspace has not yet been established. The public scripts prove the path contract and artifact reuse, but not where an independent user can clone that exact wrapper tree. Do not substitute stock upstream RPCS3 without checking for `toolchain/bin/ps4-cc`, `ps4-c++` and the matching PS4 patches.

## 5. Concrete recovery/validation sequence

Do this in an isolated candidate directory first; do not overwrite an existing WinePS4 sysroot:

```sh
mkdir -p "$HOME/wineps4-sdk-candidate"
cd "$HOME/wineps4-sdk-candidate"
curl -fL 'https://github.com/iHaiDeeZ/SMW/releases/download/sdk-1/SMW-PS4-SDK-1.tar.xz' -o sdk.tar.xz
echo '15e7927cb01fa877c04ef59beca33907cb7fe3d39b035e3e296661c3a0f2e026  sdk.tar.xz' | sha256sum -c -
tar -xJf sdk.tar.xz
```

Then inspect, rather than immediately activating, the candidate:
- Confirm `sysroot/include/c++/v1`, `sysroot/lib/libc++.a`, `sysroot/lib/libc++abi.a`, `sysroot/lib/libz.a`, `sysroot/lib/libcrypto.a` and `sysroot/libc-overlay/`.
- Confirm all 16 paths in `build/mesa/` exist and are ordinary archives (not thin archives pointing into a missing build tree).
- Record archive hashes and `llvm-ar-21 t` output.
- Compare headers and library versions with the current WinePS4 CMake link inputs.
- Only after a clean configure/link test, decide whether this SDK can seed `$HOME/wineps4-build`.

Install the PacBrew/OpenOrbis packages separately using the verified package database route in `fetch-pacbrew.sh`; do not assume the SDK archive includes `create-fself`, `PkgTool.Core` or the complete OpenOrbis directory.

## 6. Remaining high-value searches

1. Find the public origin/revision for `$HOME/rpcs3/toolchain/bin/ps4-cc` and `ps4-c++`; inspect commit history and submodules before deciding to recreate wrappers.
2. Find the RPCS3 build recipe that produced `$HOME/rpcs3-build/sysroot/lib/libavcodec.a` (M33 identifies `scripts/build-deps.sh ffmpeg`).
3. Recover the exact FreeType 2.13.3 PS4 recipe, then compare it with the existing PS4MC recipe and PacBrew's older 2.10.1 PKGBUILD.
4. Validate the public SDK candidate against WinePS4's pinned kit build. The private ps4-kit commit is only abbreviated as `56b3d86` in the public build guide; exact LLVM, Mesa and archive revisions remain unproven.

No application, port, shell, Wine patch, or packaging behavior was changed for this research note.
