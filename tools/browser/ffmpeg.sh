#!/bin/bash
#
# ffmpeg.sh -- the decoders of the browser's audio and video, for TessronOS
# (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   ffmpeg.sh    FFmpeg 4.4 configured for aarch64 with no operating system
#                (newlib), only its demuxers and decoders of the formats a page
#                plays, built into $OBJ/ffmpeg/lib*.a, headers under
#                $OBJ/ffmpeg/include
#
# FFMPEG is the source (a release tarball unpacked); without threads, the
# assembly left out (the C versions of everything).
#
set -e
FFMPEG=${FFMPEG:-$HOME/ffbt/ffmpeg-4.4.6}
OBJ=${OBJ:-$HOME/tessronos-obj}
XB=${XB:-$HOME/.local/xPacks/xpack-aarch64-none-elf-gcc-15.2.1-1.1/bin}
BR=$(cd "$(dirname "$0")/../.." && pwd)
TS=${TESSRONOS:-$(cd "$BR/../TessronOS" && pwd)}
B=$OBJ/ffmpeg/build
mkdir -p "$B"
cd "$B"
if [ ! -f config.h ]; then
	"$FFMPEG/configure" --prefix="$OBJ/ffmpeg" \
		--cc="$XB/aarch64-none-elf-gcc" --ar="$XB/aarch64-none-elf-ar" \
		--ranlib="$XB/aarch64-none-elf-ranlib" --nm="$XB/aarch64-none-elf-nm" \
		--host-cc=/usr/bin/gcc --enable-cross-compile --target-os=none --arch=aarch64 \
		--extra-cflags="-march=armv8.2-a -mno-outline-atomics -ffunction-sections -fdata-sections -D_GNU_SOURCE -I$TS/include/tsposix" \
		--extra-ldflags="--specs=nosys.specs" \
		--disable-asm --disable-inline-asm --disable-everything --disable-network \
		--disable-autodetect --disable-programs --disable-doc --disable-avdevice \
		--disable-avfilter --disable-swscale --enable-swresample --disable-postproc \
		--disable-pthreads --disable-debug --disable-runtime-cpudetect \
		--enable-demuxer='mov,matroska,ogg,mp3,wav,aac,flac,mpegts' \
		--enable-decoder='h264,vp8,vp9,theora,vp3,mpeg4,aac,mp3,vorbis,opus,flac,pcm_s16le,pcm_s24le,pcm_f32le,pcm_u8' \
		--enable-parser='h264,vp8,vp9,mpegaudio,aac,vorbis,opus,flac,vp3'
fi
make -j"${JOBS:-8}"
make install
ls -la "$OBJ/ffmpeg/lib"
