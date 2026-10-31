#!/bin/sh
# Runs the same game, input and frames natively and sandboxed and compares
# every line. Usage: cmp-flavours.sh <frames> [harness options...]
# (llvmpipe gives OpenGL 4.5; Eden asks for 4.6, which Mesa can be told to
# report.)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
S="$root/build/runs"; C="$root/build/content"
frames="$1"; shift
game="$(ls "$C"/*.nsp | head -1)"
fw="--firmware prod.keys=$C/prod.keys --firmware title.keys=$C/title.keys --firmware firmware.zip=$(ls "$C"/Firmware*.zip | head -1)"
export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460
unset DISPLAY
"$root/build/native/bin/eden-run-native" "$game" --work "$S/cmp-n-work" $fw --frames "$frames" --timeout 1800 --digest-every 10 "$@" >"$S/cmp-n.out" 2>"$S/cmp-n.err"
rn=$?
"$root/build/wbx/run-wbx" "$root/build/wbx/core.wbx" "$game" --work "$S/cmp-w-work" $fw --frames "$frames" --timeout 1800 --digest-every 10 "$@" >"$S/cmp-w.out" 2>"$S/cmp-w.err"
rw=$?
echo "native exit $rn, sandbox exit $rw, $(grep -o 'video=[0-9a-f]*' "$S/cmp-w.out" | sort -u | wc -l) pictures"
if cmp -s "$S/cmp-n.out" "$S/cmp-w.out"; then echo "NATIVE == SANDBOX in all $frames frames"; else diff "$S/cmp-n.out" "$S/cmp-w.out" | head -5 | cut -c1-170; fi
