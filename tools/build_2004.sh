#!/usr/bin/env bash
#
# Build laytonloader against Ubuntu 20.04's glibc (2.31), so it runs on older
# devices as well as new ones. Meant to be run inside the container from
# Dockerfile.build2004:
#
#   docker build -f Dockerfile.build2004 -t bogodroid-build2004 .
#   docker run --rm -v "$PWD:/src" -w /src bogodroid-build2004 ./tools/build_2004.sh
#
# FFmpeg is rebuilt too: static archives still reference glibc, so one built
# against a newer libc would drag newer symbol versions into the loader.
#
# Output: build_2004/laytonloader (the normal build_cross/ is left alone).

set -euo pipefail

PROJ="${PROJ:-laytonloader}"
repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"

echo "== toolchain =="
aarch64-linux-gnu-gcc --version | head -1

if [ "$PROJ" = "laytonloader" ]; then
    echo
    echo "== FFmpeg (rebuilding against this glibc) =="
    rm -rf projects/laytonloader/third_party/ffmpeg-min
    ./projects/laytonloader/tools/build_ffmpeg_min.sh
    rm -rf projects/laytonloader/third_party/ffmpeg-min/share
fi

echo
echo "== loader =="
rm -rf build_2004
mkdir -p build_2004
cd build_2004
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DPROJ="$PROJ" ..
make -j"$(nproc)"

echo
echo "== result =="
file "$PROJ"
echo
echo "highest glibc symbol version required:"
readelf -V "$PROJ" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -1
echo
echo "shared libraries needed:"
readelf -d "$PROJ" | awk -F'[][]' '/NEEDED/{print "  " $2}'
