#!/usr/bin/env bash
# WinePS4's FFmpeg: image decoders only, for the desktop's pictures that stb_image can't read (WebP,
# HEIC, TIFF, JPEG 2000, QOI, DDS, SGI, PCX, ...). Static, into ~/wineps4-build/ffmpeg-img (app/CMakeLists.txt
# links it). Source: FFmpeg 7.1.1 from the RPCS3 port's tree; toolchain wrappers: RPCS3's. Run in WSL.
# (no zlib: the PNG decoder is left out, stb reads PNG)
set -euo pipefail
SRC="$HOME/rpcs3-build/src/deps/ffmpeg"
TC="$HOME/rpcs3/toolchain/bin"
OUT="$HOME/wineps4-build/ffmpeg-img"
B="$HOME/wineps4-build/ffmpeg-img-build"
mkdir -p "$B"
cd "$B"
"$SRC/configure" --prefix="$OUT" --enable-cross-compile --target-os=freebsd --arch=x86_64 --cpu=btver2 \
    --cc="$TC/ps4-cc" --cxx="$TC/ps4-c++" --ld="$TC/ps4-cc" --ar=llvm-ar-21 --ranlib=llvm-ranlib-21 --nm=llvm-nm-21 \
    --strip=llvm-strip-21 --pkg-config=false --enable-static --disable-shared --enable-pic --disable-programs \
    --disable-doc --disable-network --disable-autodetect --disable-iconv --x86asmexe=nasm --disable-debug \
    --disable-everything --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --disable-swresample \
    --disable-avdevice --disable-avfilter \
    --enable-decoder=png,apng,bmp,gif,webp,tiff,hevc,mjpeg,jpeg2000,targa,ppm,pgm,pgmyuv,pbm,pam,qoi,sgi,pcx,dds \
    --enable-demuxer=image2,image_png_pipe,image_bmp_pipe,image_gif_pipe,gif,image_webp_pipe,image_tiff_pipe,image_jpeg_pipe,image_j2k_pipe,image_ppm_pipe,image_pgm_pipe,image_pbm_pipe,image_pam_pipe,image_qoi_pipe,image_sgi_pipe,image_pcx_pipe,image_dds_pipe,mov,hevc \
    --enable-parser=png,bmp,gif,webp,hevc,mjpeg,jpeg2000,pnm --enable-protocol=file --extra-cflags=-O2 \
    > "$HOME/wineps4-build/ffmpeg-img-configure.log" 2>&1
make -j"$(nproc)" > "$HOME/wineps4-build/ffmpeg-img-build.log" 2>&1
make install >> "$HOME/wineps4-build/ffmpeg-img-build.log" 2>&1
ls -la "$OUT/lib"
