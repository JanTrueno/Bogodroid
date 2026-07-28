#!/usr/bin/env bash
#
# Build a minimal static FFmpeg for aarch64, for Laytonloader's cutscenes.
#
# The distro packages drag in ~133 shared libraries (AV1, RabbitMQ, cairo, ...)
# because they are built against every codec FFmpeg supports, and the dynamic
# linker has to resolve all of them even though we decode exactly two things.
# Ubuntu's static libs are no better: they reference the same externals, so
# linking them needs 1500+ symbols that no .a on the system provides.
#
# This builds only what the .mp4 cutscenes need -- H.264 video, AAC audio, the
# mov demuxer -- with --disable-autodetect so configure cannot quietly pick up
# system codec libraries. The result links straight into the loader: no libav*
# .so files to ship at all.
#
# Usage:  tools/build_ffmpeg_min.sh [version]
#
# Installs to dist/ffmpeg-min/, which CMakeLists.txt picks up automatically.

set -euo pipefail

VERSION="${1:-6.1.1}"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
prefix="$repo_root/projects/laytonloader/third_party/ffmpeg-min"
work="${TMPDIR:-/tmp}/bogodroid-ffmpeg-$VERSION"

command -v "${CROSS_PREFIX}gcc" >/dev/null || {
    echo "error: ${CROSS_PREFIX}gcc not found (apt install g++-aarch64-linux-gnu)" >&2
    exit 1
}

mkdir -p "$work"
cd "$work"

tarball="ffmpeg-$VERSION.tar.xz"
if [ ! -f "$tarball" ]; then
    echo ">> downloading $tarball"
    curl -sSL -o "$tarball" "https://ffmpeg.org/releases/$tarball"
fi

if [ ! -d "ffmpeg-$VERSION" ]; then
    echo ">> extracting"
    tar -xf "$tarball"
fi

cd "ffmpeg-$VERSION"

echo ">> configuring (prefix: $prefix)"
./configure \
    --prefix="$prefix" \
    --enable-cross-compile --arch=aarch64 --target-os=linux \
    --cross-prefix="$CROSS_PREFIX" \
    --disable-everything --disable-autodetect \
    --disable-programs --disable-doc \
    --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-network \
    --disable-shared --enable-static --enable-pic \
    --enable-decoder=h264 --enable-decoder=aac \
    --enable-demuxer=mov \
    --enable-parser=h264 --enable-parser=aac \
    --enable-protocol=file

echo ">> building"
make -j"$(nproc)"
make install

echo
echo "installed to $prefix"
ls -lh "$prefix"/lib/*.a
echo
echo "Re-run cmake in your build dir so it picks this up:"
echo "  cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-linux-gnu.cmake -DPROJ=laytonloader .."
