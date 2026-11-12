# Building WinePS4

WinePS4 builds on **ps4-kit**, the shared PS4 base of these ports, which isn't published yet. v00.71 was built
with ps4-kit `56b3d86`. The steps below are how the release package is made, so the Wine changes and the build stay
open to read.

Everything runs in WSL (Ubuntu), with this repository at `~/wineps4` and ps4-kit at `~/ps4-kit`.

1. **Wine 11.0 with the PS4 patch**, at `~/wine-ps4-build/src/wine`:

   ```bash
   git clone --branch wine-11.0 --depth 1 https://gitlab.winehq.org/wine/wine.git ~/wine-ps4-build/src/wine
   cd ~/wine-ps4-build/src/wine && git am ~/wineps4/patches/wine-ps4.patch
   ```

2. **The PC's Wine build**, which makes the Windows side: Wine's PE DLLs and programs (64-bit and 32-bit), the
   headers made from IDL files, and the tools. In `~/wine-ps4-build/build/wine-wow64`:

   ```bash
   ../../src/wine/configure --enable-archs=i386,x86_64 --prefix=$HOME/wine-ps4-build/install/wine-wow64 \
       --without-x --without-wayland --without-freetype --without-gstreamer --without-pulse --without-alsa \
       --without-oss --without-cups --without-dbus --without-sane --without-krb5 --without-gphoto \
       --without-opencl --without-pcap --without-usb --without-v4l2 --without-netapi --without-gnutls \
       --without-sdl --without-udev --without-capi --without-pcsclite --without-ffmpeg --without-opengl \
       --without-fontconfig
   make -j$(nproc) && make install
   ```

3. **The prefix** the package carries, made by that Wine:
   `WINEPREFIX=$HOME/wine-ps4-build/prefix-wow64 $HOME/wine-ps4-build/install/wine-wow64/bin/wine wineboot --init`.

4. **The PS4's libraries** in `~/wineps4-build`: FreeType 2.13.3, static, in `freetype/` (built with ps4-kit's
   toolchain), and FFmpeg's image decoders in `ffmpeg-img/` (`scripts/build-ffmpeg-img.sh`).

5. **DXVK 3.1.1**: its release from [github.com/doitsujin/dxvk](https://github.com/doitsujin/dxvk/releases),
   unpacked to `~/wineps4-build/dxvk-3.1.1`.

6. **The test programs**: `tests/build.sh` (mingw-w64, and the Wine build's `winegcc` for the 32-bit one).

7. **The app and its package**:

   ```bash
   cd ~/wineps4
   WINEPS4_VERSION=00.71 app/build.sh all
   WINEPS4_VERSION=00.71 app/package.sh
   ```

   `app/build.sh` compiles Wine's Unix side (ntdll, wineserver, win32u, `wineps4.drv` and the other modules' Unix
   halves) with `port/` and the desktop in `shell/` into one PS4 executable. `app/package.sh` puts it in
   `out/WinePS4-vXX.YY.pkg` with the Windows side, the prefix, DXVK, the fonts and the root certificates.

How the port works, and what changed in each version: [NOTES.md](../NOTES.md).
