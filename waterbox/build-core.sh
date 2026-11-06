#!/bin/sh
# Links core.wbx - Eden as a Chimera waterbox core - from the guest CMake
# archives (build-guest.sh) and the guest's libraries (build-deps.sh), and
# builds run-wbx, the host runner the gate uses.
#
# Prereq: a miniBox checkout built WITH the C++ guest toolchain:
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true
#   ninja -C <miniBox>/build/meson-cpp
#
# Usage: ./build-core.sh [-m <miniBox dir>] [-o <output dir>]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/chimera-common-minibox}"
out="$root/build/wbx"
while getopts "m:o:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		o) out="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"
deps="$root/build/deps"
[ -f "$sr/lib/libstdc++.a" ] || { echo "miniBox C++ guest toolchain missing at $sr." >&2; exit 1; }
[ -d "$root/build/guest" ] || { echo "build/guest missing - run build-guest.sh first." >&2; exit 1; }
mkdir -p "$out"

# the core's own archive whole (nothing references the exports), then one
# group: the CMake archives cross-reference freely. The archives are thin
# (Eden's CMake makes them so), which the linker reads as they are.
core_a="$(find "$root/build/guest" -name libeden-guest-core.a | head -1)"
[ -n "$core_a" ] || { echo "libeden-guest-core.a missing - run build-guest.sh first." >&2; exit 1; }
libs="$(find "$root/build/guest" -name '*.a' ! -name libeden-guest-core.a | sort | tr '\n' ' ')"
deplibs="$deps/ffmpeg-guest/lib/libavfilter.a $deps/ffmpeg-guest/lib/libswscale.a $deps/ffmpeg-guest/lib/libavcodec.a $deps/ffmpeg-guest/lib/libavutil.a $deps/openssl-guest/lib/libssl.a $deps/openssl-guest/lib/libcrypto.a"
g++ -specs "$sr/lib/musl-gcc.specs" -mcmodel=large -fno-pic -fno-pie \
	-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax,-z,stack-size=8388608 -T "$mb/source/guest/linkscript.T" \
	-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create \
	-Wl,-u,pthread_mutexattr_init -Wl,-u,pthread_mutexattr_settype -Wl,-u,pthread_mutexattr_destroy \
	-o "$out/core.wbx" \
	"$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
	-Wl,--whole-archive "$core_a" -Wl,--no-whole-archive \
	-Wl,--start-group $libs $deplibs -Wl,--end-group \
	-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc
sh "$mb/source/guest/check-wbx.sh" "$out/core.wbx"
echo "built $out/core.wbx"

# the host runner for the gate
mbhost="${MINIBOX_HOST_DIR:-$mb/build/meson-linux/source/host}"
[ -f "$mbhost/libminiboxhost.so" ] || mbhost="$mbuild/source/host"
# (the Vulkan bridge's host half is built apart: it wants the Vulkan headers
# it was generated from)
gcc -O2 -g -Wall -c -I"$mb/source/vk" -I"$mb/source/host" -I"$root/build/generated-vk" \
	-I"$mb/extern/vulkan-headers/include" -o "$out/vk-host.o" "$mb/source/vk/vk-host.c"
gcc -O2 -g -Wall -DCHIMERA_GL_BRIDGE -I"$here" -I"$mb/source/host" -I"$mb/source/gl" \
	-I"$here/glad/include" -I"$root/build/generated-gl" \
	-o "$out/run-wbx" "$here/run-wbx.c" "$here/gl-host.c" "$here/glad/src/gl.c" "$out/vk-host.o" \
	"$mbhost/libminiboxhost.so" -Wl,-rpath,"$mbhost" -lEGL -ldl
echo "built $out/run-wbx"
