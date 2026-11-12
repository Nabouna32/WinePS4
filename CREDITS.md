# Credits

WinePS4 is a port. Nearly everything that runs the programs is other people's work.

## Wine and the Windows side

- **[Wine](https://www.winehq.org)** 11.0 by the Wine project (LGPL-2.1-or-later): runs the Windows programs.
  WinePS4 applies `patches/wine-ps4.patch` (46 commits) to Wine's `wine-11.0` tag. The patch's new files, the PS4
  display, input and audio driver `wineps4.drv` and the server's PS4 input, are also in `wine-overlay/`.
- **[DXVK](https://github.com/doitsujin/dxvk)** 3.1.1 by Philip Rebohle and contributors (zlib): Direct3D 8, 9, 10
  and 11 on Vulkan. Its release DLLs are in the package, unchanged.
- **[FreeType](https://freetype.org)** 2.13.3 (FreeType License): the fonts of Wine's win32u and DirectWrite.
- **[FFmpeg](https://ffmpeg.org)** 7.1.1 (LGPL-2.1-or-later): its image decoders only, for the desktop's pictures
  that stb can't read (`scripts/build-ffmpeg-img.sh`).
- **[OpenSSL](https://www.openssl.org)** (Apache-2.0) and **[zlib](https://zlib.net)**: the sysroot's libcrypto,
  under bcrypt's PS4 half, and zlib.
- **[stb](https://github.com/nothings/stb)** by Sean Barrett (public domain / MIT): `stb_image` and
  `stb_truetype` in the desktop (`shell/third_party`).

## The PS4 side

- **[Mesa](https://mesa3d.org)** (MIT): the RADV Vulkan driver, ported to the PS4's GPU, which games' Vulkan and
  DXVK draw with.
- **ps4-kit** by ShiroKlein: the shared PS4 base of these ports (runtime, heap, threads, the RADV port and its
  window system, packaging), merged from [Resonance](https://github.com/iHaiDeeZ/Resonance),
  [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4), [M33](https://github.com/iHaiDeeZ/M33),
  [Crema](https://github.com/iHaiDeeZ/Crema) and the other ports. It isn't published yet.
- **[OpenOrbis](https://github.com/OpenOrbis)** (GPL-3.0 toolchain; its libraries under their own terms) and
  **[PacBrew](https://github.com/PacBrew)**: the PS4 toolchains.
- **[LLVM](https://llvm.org)** (Apache-2.0 WITH LLVM-exception): clang and libc++.
- **[GoldHEN](https://github.com/GoldHEN/GoldHEN)**: the homebrew enabler WinePS4 runs under.

## In the package

- **Liberation** fonts 2.1.5 (SIL Open Font License 1.1, `app/windows-fonts/LICENSE-Liberation.txt`): stand-ins
  for Arial, Times New Roman and Courier New with the same metrics, for Windows programs.
- **Roboto** (SIL Open Font License 1.1, `app/fonts/OFL-Roboto.txt`): the desktop's text.
- **Mozilla's root certificates** (MPL 2.0, from Ubuntu's ca-certificates): `app/certs/ca-certificates.crt`, the
  certificates Windows programs trust.
- The wallpapers (`scripts/make-wallpapers.py`), the icon (`scripts/make-icon.py`) and this repository's banner
  (`scripts/make-banner.py`) are drawn by scripts in this repository.
- No Windows files, programs or games: Wine's own DLLs stand in for Windows', and you bring your own programs.

## Thanks

To the Wine developers for decades of making Windows programs run everywhere else, to the DXVK and Mesa developers,
to Valve's Proton, whose defaults WinePS4 follows in places, and to everyone keeping the PS4 homebrew scene and its
toolchains alive.
