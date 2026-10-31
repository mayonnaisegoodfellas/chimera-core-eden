#!/bin/bash
# The frontend test: the package in Chimera's own window, run headless (no
# display is needed), against the same project run through Chimera's engine
# by chimera-run.
#
#   project:frontend  a project of the game, with a movie that presses A, is
#                     opened in the window (in TAStudio) and sought to a
#                     frame; the picture there must be the one chimera-run
#                     draws at that frame of the same project, pixel for pixel
#   keybinds          the bindings the package ships are the machine's when
#                     nobody changed them
#
# It needs what the test script's game tests need, in build/content (or
# EDEN_CONTENT): one .nsp or .xci, prod.keys, and title.keys and a
# Firmware*.zip when there are some. A Switch game cannot start without the
# console's keys, and none of them may be put in a repository, so without
# them every test says SKIP.
#
# Usage: waterbox/tests/run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
# 340: the movie's A at frame 300 has closed the first dialog by then (the
# picture changes at 325), and the screen has not yet faded out (350)
frames=340
chimera_root="${CHIMERA_ROOT:-}"
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$chimera_root" ] || chimera_root="$HOME/chimera"
[ -d "$chimera_root" ] || { echo "Chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
crun="$chimera_root/build/meson-linux/chimera-run"
package="$root/build/package/eden.chimeraCore"
[ -x "$chimera_exe" ] || { echo "Chimera's window is not built: $chimera_exe" >&2; exit 1; }
[ -x "$crun" ] || { echo "chimera-run is not built: $crun" >&2; exit 1; }
[ -f "$package" ] || { echo "no package: $package (run waterbox/build-package.sh)" >&2; exit 1; }

ok=0
failed=0
skipped=0
report() {
	printf "%-20s %-5s %s\n" "$1" "$2" "$3"
	case "$2" in
		PASS) ok=$((ok + 1)) ;;
		SKIP) skipped=$((skipped + 1)) ;;
		*) failed=$((failed + 1)) ;;
	esac
}
finish() {
	echo
	echo "$ok ok, $failed failed, $skipped skipped"
	[ "$failed" -eq 0 ]
	exit
}

content="${EDEN_CONTENT:-$root/build/content}"
game="$(ls "$content"/*.nsp "$content"/*.xci 2>/dev/null | head -1)"
firmware="$(ls "$content"/Firmware*.zip "$content"/firmware*.zip 2>/dev/null | head -1)"
if [ -z "$game" ] || [ ! -f "$content/prod.keys" ]; then
	report "project:frontend" SKIP "no .nsp/.xci or no prod.keys in $content"
	report "keybinds" SKIP "needs a game to open"
	finish
fi

work="$root/build/frontend"
rm -rf "$work"
mkdir -p "$work/cores" "$work/firmware"

# The window finds Qt where Chimera's tools/ci-chimera.sh puts it (or QT)
qt_lib=""
for q in "${QT:-}" "$HOME"/qt/*/gcc_64; do
	[ -n "$q" ] && [ -f "$q/lib/libQt6Core.so.6" ] && { qt_lib="$q/lib"; break; }
done
[ -n "$qt_lib" ] && export LD_LIBRARY_PATH="$qt_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Eden draws with OpenGL 4.6; Mesa's software renderer offers 4.5 unless told
# otherwise (a real graphics card's driver ignores these)
export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460
# The window with nothing of an installed Chimera's: this package alone in its
# cores folder, its settings, cache and recovery files the run's own
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$work/cores" CHIMERA_FIRMWARE_DIR="$work/firmware" \
	CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"
cp "$package" "$work/cores/eden.chimeraCore"

# The firmware as a person's Firmware folder holds it, under the names the
# package declares; the game beside the project, where a project looks for it.
# Links to build/content, which is a copy and never the originals.
fw=(--firmware "prod.keys=$content/prod.keys")
pin=("--firmware=prod.keys=$content/prod.keys")
ln -s "$content/prod.keys" "$work/firmware/prod.keys"
if [ -f "$content/title.keys" ]; then
	fw+=(--firmware "title.keys=$content/title.keys")
	pin+=("--firmware=title.keys=$content/title.keys")
	ln -s "$content/title.keys" "$work/firmware/title.keys"
fi
if [ -n "$firmware" ]; then
	fw+=(--firmware "firmware.zip=$firmware")
	pin+=("--firmware=firmware.zip=$firmware")
	ln -s "$firmware" "$work/firmware/firmware.zip"
fi
ln -s "$game" "$work/$(basename "$game")"

# The project: the game, the firmware pinned, and a movie that presses A
# (tests/press-a.txt: the first screen of the game the tests were made with
# is a dialog that waits for it)
project="$work/frontend.chimeraProject"
if ! python3 "$here/make-project.py" "$package" "$project" "$here/press-a.txt" "$frames" "${pin[@]}" \
	"game=$work/$(basename "$game")" > "$work/make.log" 2>&1; then
	report "project:frontend" FAIL "the project was not made: $(tail -1 "$work/make.log")"
	report "keybinds" SKIP "needs the project"
	finish
fi

# --- 1. the window's picture is the engine's ---------------------------------
# chimera-run counts frames from 0: the picture after the last of N frames is
# frame N-1's (a TGA, whatever the name says)
"$crun" --project "$project" "$package" --files "$work" --gpu "${fw[@]}" \
	--screenshot "$((frames - 1))=$work/engine.tga" > "$work/engine.log" 2>&1
r=$?
printf 'frames=%s\nshot=%s/window.png\nmeta=%s/window.meta.txt\n' "$frames" "$work" "$work" > "$work/job.window.txt"
echo '{}' > "$work/settings.json"
( cd "$work" && MINIHAWK_JOB="$work/job.window.txt" timeout 1800 "$chimera_exe" --headless \
	"--config=$work/settings.json" "--project=$project" "--lua=$here/tastudio-shot.lua" ) > "$work/window.log" 2>&1
if [ "$r" != 0 ] || [ ! -s "$work/engine.tga" ]; then
	report "project:frontend" FAIL "chimera-run gave no picture at frame $frames (exit $r): $(tail -1 "$work/engine.log" | cut -c1-120)"
elif [ ! -f "$work/window.meta.txt" ] || ! grep -q '^status=OK' "$work/window.meta.txt"; then
	report "project:frontend" FAIL "the window did not report OK - a dialog, a stall or a crash (see build/frontend/window.log)"
elif ! grep -q '^tastudio=true' "$work/window.meta.txt"; then
	report "project:frontend" FAIL "the project did not open in TAStudio"
elif ! same="$(python3 -I "$here/same-picture.py" "$work/engine.tga" "$work/window.png" 2>&1)"; then
	report "project:frontend" FAIL "the window's picture at frame $frames is not chimera-run's: $same"
else
	report "project:frontend" PASS "the project opened in TAStudio and sought to frame $frames shows chimera-run's picture there ($same)"
fi

# --- 2. the bindings the package ships are the machine's ---------------------
# (the window keeps only the keys a person changed; fresh settings have none)
printf 'out=%s/keys.controls.json\nmeta=%s/keys.meta.txt\n' "$work" "$work" > "$work/job.keys.txt"
( cd "$work" && MINIHAWK_JOB="$work/job.keys.txt" timeout 900 "$chimera_exe" --headless \
	"--config=$work/settings.json" "--project=$project" "--lua=$here/dump-controls.lua" ) > "$work/keys.log" 2>&1
if [ ! -f "$work/keys.meta.txt" ] || ! grep -q '^status=OK' "$work/keys.meta.txt"; then
	report "keybinds" FAIL "the bindings were not written (see build/frontend/keys.log)"
elif out="$(python3 -I "$here/check-keybinds.py" "$work/keys.controls.json" "$wb/default_keybinds.json" "Nintendo Switch" 2>&1)"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

finish
