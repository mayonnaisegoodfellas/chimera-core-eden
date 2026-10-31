#!/bin/sh
# Builds what Eden needs and does not carry: FFmpeg (the video decoders of
# the Switch's NVDEC: H.264, VP8, VP9), static, without assembly and without
# threads, so it decodes the same on every host.
#
# Usage: build-deps.sh native|guest
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
flavour="${1:?native|guest}"
deps="$root/build/deps"
jobs="$(nproc)"
mkdir -p "$deps"

FFMPEG_URL=https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz
FFMPEG_SHA256=089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz
OPENSSL_SHA256=23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533

SR="${MINIBOX_SYSROOT:-${MINIBOX_DIR:-$HOME/chimera/extern/chimera-common-minibox}/build/meson-cpp/guest-sysroot}"
# (x86-64-v2: see build-native.sh)
ARCH_CFLAGS="-march=x86-64-v2 -mtune=generic"
GUEST_CFLAGS="-fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -O2"

fetch() {  # <url> <sha256> <file>
	[ -f "$deps/$3" ] || curl -sL -o "$deps/$3" "$1"
	echo "$2  $deps/$3" | sha256sum -c - >/dev/null
}

out="$deps/ffmpeg-$flavour"
if [ ! -f "$out/lib/libavcodec.a" ]; then
	fetch "$FFMPEG_URL" "$FFMPEG_SHA256" ffmpeg-7.1.2.tar.xz
	src="$deps/ffmpeg-7.1.2"
	[ -d "$src" ] || tar xJf "$deps/ffmpeg-7.1.2.tar.xz" -C "$deps"
	bld="$deps/ffmpeg-$flavour-obj"
	rm -rf "$bld"; mkdir -p "$bld"
	if [ "$flavour" = guest ]; then
		cc="gcc -specs=$SR/lib/musl-gcc.specs"
		cflags="$GUEST_CFLAGS $ARCH_CFLAGS"
		cross="--enable-cross-compile --target-os=linux --arch=x86_64 --pkg-config=false"
	else
		cc=gcc
		cflags="-O2 $ARCH_CFLAGS"
		cross="--enable-pic"
	fi
	(cd "$bld" && "$src/configure" --prefix="$out" --cc="$cc" --extra-cflags="$cflags" $cross \
		--disable-asm --disable-x86asm --disable-inline-asm --disable-programs --disable-doc \
		--disable-network --disable-shared --enable-static --disable-autodetect --disable-debug \
		--disable-everything --disable-avdevice --disable-avformat --disable-swresample \
		--enable-avcodec --enable-avfilter --enable-swscale \
		--enable-decoder=h264,vp8,vp9 --enable-parser=h264,vp8,vp9 \
		--enable-filter=yadif,buffer,buffersink \
		--disable-pthreads --disable-w32threads --disable-os2threads \
		--disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2 --disable-xlib \
		--disable-runtime-cpudetect) > "$bld/configure.log" 2>&1 || { tail -20 "$bld/configure.log"; exit 1; }
	make -C "$bld" -j"$jobs" > "$bld/make.log" 2>&1 || { tail -20 "$bld/make.log"; exit 1; }
	make -C "$bld" install > "$bld/install.log" 2>&1
fi
echo "ffmpeg-$flavour: $(ls "$out/lib" | tr '\n' ' ')"

[ "$flavour" = guest ] || exit 0

# OpenSSL for the guest (the console's cryptography: its keys, NCA headers,
# AES-XTS; the SSL service never reaches a network): no assembly, no
# threads, no engines.
out="$deps/openssl-guest"
if [ ! -f "$out/lib/libcrypto.a" ]; then
	fetch "$OPENSSL_URL" "$OPENSSL_SHA256" openssl-3.0.15.tar.gz
	bld="$deps/openssl-guest-obj"
	rm -rf "$bld"; mkdir -p "$bld"
	tar xzf "$deps/openssl-3.0.15.tar.gz" -C "$bld" --strip-components=1
	(cd "$bld" && CC="gcc -specs=$SR/lib/musl-gcc.specs" ./Configure linux-x86_64 --prefix="$out" --libdir=lib \
		no-shared no-asm no-threads no-dso no-engine no-async no-tests \
		no-module no-legacy no-autoload-config no-secure-memory no-afalgeng no-devcryptoeng \
		$GUEST_CFLAGS) > "$bld/configure.log" 2>&1 || { tail -20 "$bld/configure.log"; exit 1; }
	make -C "$bld" -j"$jobs" build_libs > "$bld/make.log" 2>&1 || { tail -20 "$bld/make.log"; exit 1; }
	make -C "$bld" install_dev > "$bld/install.log" 2>&1
fi
echo "openssl-guest: $(ls "$out"/lib/ 2>/dev/null | grep -E '\.a$' | tr '\n' ' ')"

