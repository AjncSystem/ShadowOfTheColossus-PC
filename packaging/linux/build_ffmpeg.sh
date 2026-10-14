#!/usr/bin/env bash
# Builds the minimal FFmpeg the port needs into out/ffmpeg: the game's videos only (MP4 with
# H.264/HEVC video and AAC audio), as shared libraries with no dependencies beyond libc/libm.
# Distribution FFmpeg packages either lack these decoders (Fedora's ffmpeg-free) or pull in
# about a hundred libraries an AppImage would have to carry. build.sh uses out/ffmpeg when present.
#   bash packaging/linux/build_ffmpeg.sh        (needs git, a C compiler, make and nasm)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
prefix=$PWD/out/ffmpeg
version=${FFMPEG_VERSION:-n7.1}
for tool in git make nasm cc; do
    command -v "$tool" >/dev/null || { echo "Need $tool (packages: git make nasm gcc)." >&2; exit 1; }
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git clone --quiet --depth 1 --branch "$version" https://github.com/FFmpeg/FFmpeg.git "$work/ffmpeg"
cd "$work/ffmpeg"
./configure --prefix="$prefix" --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-network --disable-autodetect --disable-everything \
    --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
    --enable-protocol=file --enable-demuxer=mov,h264,hevc,aac \
    --enable-decoder=h264,hevc,aac --enable-parser=h264,hevc,aac \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc >/dev/null
make -j"$(nproc)" >/dev/null
rm -rf "$prefix"
make install >/dev/null
echo "FFmpeg $version in $prefix. Rebuild the GPU library with it: rm -rf out/gpu && ./build.sh"
