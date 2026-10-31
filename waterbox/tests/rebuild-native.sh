#!/bin/sh
# A renderer rebuilt before every frame must draw what an untouched one
# draws: runs the game natively both ways and compares every line.
# Usage: rebuild-native.sh <frames> [harness options...]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
S="$root/build/runs"; C="$root/build/content"
frames="$1"; shift
game="$(ls "$C"/*.nsp | head -1)"
fw="--firmware prod.keys=$C/prod.keys --firmware title.keys=$C/title.keys --firmware firmware.zip=$(ls "$C"/Firmware*.zip | head -1)"
export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 MESA_DEBUG=1
unset DISPLAY
run="$root/build/native/bin/eden-run-native"
"$run" "$game" --work "$S/pn-work" $fw --frames "$frames" --timeout 1800 "$@" >"$S/pn.out" 2>"$S/pn.err"
echo "plain exit $?"
"$run" "$game" --work "$S/rn-work" $fw --frames "$frames" --timeout 1800 --rebuild-every-frame "$@" >"$S/rn.out" 2>"$S/rn.err"
echo "rebuild exit $?"
if cmp -s "$S/pn.out" "$S/rn.out"; then
	echo "REBUILT == PLAIN in all $frames frames ($(grep -o 'video=[0-9a-f]*' "$S/pn.out" | sort -u | wc -l) pictures)"
else
	echo "$(diff "$S/pn.out" "$S/rn.out" | grep -c '^>') lines differ"
	diff "$S/pn.out" "$S/rn.out" | grep '^[<>]' | head -4 | cut -c1-170
fi
grep "Mesa: error" "$S/rn.err" | sort | uniq -c | sort -rn | head -8
