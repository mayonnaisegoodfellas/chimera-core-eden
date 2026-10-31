#!/bin/sh
# Builds the native reference (build/native/bin/eden-run-native): Eden's
# libraries with CHIMERA_HEADLESS, our driver in place of the frontends.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
build="$root/build/native"
# Eden wants CMake 3.31; build/deps holds one when the system's is older
cmake="${CMAKE:-cmake}"
[ -x "$root/build/deps/cmake-whl/cmake/data/bin/cmake" ] && cmake="$root/build/deps/cmake-whl/cmake/data/bin/cmake"

# glslangValidator (Vulkan host shaders) may live in build/deps when the system has none
[ -d "$root/build/deps/apt/root/usr/bin" ] && PATH="$root/build/deps/apt/root/usr/bin:$PATH"

# The instruction set every object is built for: x86-64-v2 (SSE4.1 and the
# rest of a 2008 CPU's). Upstream builds for the 2003 baseline unless told
# otherwise, and its SSE4.1 paths (the video converter's) are then compiled
# out; its Windows builds already require SSE4.1. The same in every build of
# this core, so the same machine on every host.
arch="-march=x86-64-v2 -mtune=generic"

# The compilers by their full paths: CMake keeps them so, and given the bare
# name again it takes the compiler for changed, throws its cache away and
# configures once more with none of the options below (the headless build
# among them).
cc="$(command -v "${CC:-gcc}")"
cxx="$(command -v "${CXX:-g++}")"

sh "$here/apply-patches.sh"
sh "$here/build-deps.sh" native
sh "$here/gen-gl.sh"

"$cmake" -S "$root/extern/eden" -B "$build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -g -DNDEBUG $arch" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -g -DNDEBUG $arch" \
	-DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
	-DCHIMERA_HEADLESS=ON -DCHIMERA_DRIVER_DIR="$here/native" -DFFMPEG_DIR="$root/build/deps/ffmpeg-native" \
	-DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
	-DENABLE_CUBEB=OFF -DENABLE_LIBUSB=OFF -DENABLE_WERROR=OFF
"$cmake" --build "$build" --target eden-run-native
