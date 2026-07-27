# Cross build image for Bogodroid NEO, targeting aarch64 (ARM64) Linux.
#
# The host running `docker build`/`docker run` can be amd64 or arm64 -- this
# image is always built for linux/arm64 via QEMU emulation (buildx), so the
# compiler, linker and every apt package inside it are native aarch64.
#
# Build:
#   docker buildx build --platform linux/arm64 -t bogodroid-arm64 --load .
#   (add --build-arg PROJ=unityloader to build a different project, default: laytonloader)
#   (add --build-arg JOBS=12 to cap parallel make jobs; default uses all cores)
#
# Run (see docker-entrypoint.sh / README section for X11 details):
#   docker run --rm -it --platform linux/arm64 \
#       -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
#       -v "$(pwd)/gamefiles:/bogodroid/gamefiles" \
#       bogodroid-arm64

FROM ubuntu:22.04

ARG PROJ=laytonloader
ENV PROJ=${PROJ}
ARG JOBS
ENV DEBIAN_FRONTEND=noninteractive

# --- Build + runtime dependencies ---
# build-essential/cmake/git: toolchain
# libzip-dev, libsdl2-dev, libbsd-dev: linked by the project (see CMakeLists.txt)
# libgl1-mesa-dri/libglx-mesa0/libegl1/libgles2: GL/EGL runtime for SDL2 (software rendering via llvmpipe when no GPU passthrough is available)
# libasound2, libpulse0: SDL2 audio backends
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        libzip-dev \
        libsdl2-dev \
        libbsd-dev \
        libgl1-mesa-dri \
        libglx-mesa0 \
        libegl1 \
        libgles2 \
        libasound2 \
        libpulse0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /bogodroid

# Copy the repo (submodules must already be checked out on the host, see .dockerignore)
COPY . .

RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DPROJ=${PROJ} \
    && make -j"${JOBS:-$(nproc)}"

WORKDIR /bogodroid/build

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD []
