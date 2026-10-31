#!/bin/sh
# The guest (waterbox) build: Eden's own CMake under the miniBox musl
# toolchain, the libraries only (build/guest); build-core.sh links core.wbx.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
deps="$root/build/deps"
cmake="${CMAKE:-cmake}"
[ -x "$deps/cmake-whl/cmake/data/bin/cmake" ] && cmake="$deps/cmake-whl/cmake/data/bin/cmake"
[ -d "$deps/apt/root/usr/bin" ] && PATH="$deps/apt/root/usr/bin:$PATH"

# The instruction set every object is built for: x86-64-v2 (SSE4.1 and the
# rest of a 2008 CPU's). Upstream builds for the 2003 baseline unless told
# otherwise, and its SSE4.1 paths (the video converter's) are then compiled
# out; its Windows builds already require SSE4.1. The same in every build of
# this core, so the same machine on every host.
arch="-march=x86-64-v2 -mtune=generic"

sh "$here/apply-patches.sh"
sh "$here/build-deps.sh" guest
sh "$here/gen-gl.sh"

"$cmake" -S "$root/extern/eden" -B "$root/build/guest" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -g -DNDEBUG $arch" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -g -DNDEBUG $arch" \
	-DCHIMERA_HEADLESS=ON -DCHIMERA_DRIVER_DIR="$here/guest" -DFFMPEG_DIR="$deps/ffmpeg-guest" \
	-DOPENSSL_ROOT_DIR="$deps/openssl-guest" -DOPENSSL_USE_STATIC_LIBS=TRUE \
	-DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
	-DENABLE_CUBEB=OFF -DENABLE_LIBUSB=OFF -DENABLE_WERROR=OFF
"$cmake" --build "$root/build/guest" --target eden-guest-core "$@"
