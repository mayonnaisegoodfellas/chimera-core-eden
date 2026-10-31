#!/bin/sh
# States in the sandbox, against a plain run of the same frames:
#   rerecord  a state saved and loaded before every frame (same process)
#   resume    a state saved by one process at frame <split>, loaded by another
# Usage: states-wbx.sh <frames> <split> [harness options...]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
S="$root/build/runs"; C="$root/build/content"
frames="$1"; split="$2"; shift 2
game="$(ls "$C"/*.nsp | head -1)"
fw="--firmware prod.keys=$C/prod.keys --firmware title.keys=$C/title.keys --firmware firmware.zip=$(ls "$C"/Firmware*.zip | head -1)"
export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460
unset DISPLAY
run() { name="$1"; n="$2"; shift 2
	"$root/build/wbx/run-wbx" "$root/build/wbx/core.wbx" "$game" --work "$S/st-$name-work" $fw --frames "$n" --timeout 3600 --digest-every 1 "$@" >"$S/st-$name.out" 2>"$S/st-$name.err"
	echo "$name exit $?"; }
same() { if cmp -s "$1" "$2"; then echo "$3: SAME ($(wc -l < "$1") lines, $(grep -o 'video=[0-9a-f]*' "$1" | sort -u | wc -l) pictures)"; else echo "$3: $(diff "$1" "$2" | grep -c '^>') lines differ"; diff "$1" "$2" | grep '^[<>]' | head -4 | cut -c1-150; fi; }
run plain "$frames" "$@"
grep '^frame ' "$S/st-plain.out" > "$S/st-plain.frames"
run rerecord "$frames" --rerecord "$@"
grep '^frame ' "$S/st-rerecord.out" > "$S/st-rerecord.frames"
same "$S/st-plain.frames" "$S/st-rerecord.frames" rerecord
run first "$split" --save-state "$S/st.state" "$@"
ls -la "$S/st.state" | awk '{print "state", $5, "bytes"}'
run second "$((frames - split))" --state "$S/st.state" "$@"
awk -v s="$split" '$1 == "frame" && $2 > s' "$S/st-plain.out" > "$S/st-plain.tail"
grep '^frame ' "$S/st-second.out" > "$S/st-second.frames"
same "$S/st-plain.tail" "$S/st-second.frames" resume
grep '^loaded' "$S/st-second.out" | cut -c1-120
rm -f "$S/st.state"
