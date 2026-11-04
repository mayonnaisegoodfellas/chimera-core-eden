#!/bin/sh
# The gate: a game through the native reference and through the sandbox,
# frame by frame; a renderer made again against one left alone; states in
# the sandbox against a plain run; input, turbo and save data; and the
# negative controls that prove the comparisons can fail.
#
#   run-gate.sh [-n]    -n: do not build (use what build/ holds)
#
# A leg prints PASS, FAIL or SKIP and one line of evidence; the exit status
# is the number of failed legs. A game, the console's keys and its system
# software are nobody's to put in a repository: the legs that need them look
# in build/content (or EDEN_CONTENT) for one .nsp or .xci, prod.keys,
# title.keys and a Firmware*.zip, and SKIP, saying what they lacked.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
run="$root/build/native/bin/eden-run-native"
wbx="$root/build/wbx/run-wbx"
core="$root/build/wbx/core.wbx"
work="$root/build/gate"
# Chimera runs a game only in a project: crun_game <package>
# <game> <movie> [chimera-run's options] makes one of the game and the movie,
# with any --settings and --firmware given, and runs it with the rest.
crun_quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
crun_game() {
	cg_pkg="$1" cg_game="$2" cg_movie="$3" cg_made=""
	shift 3
	cg_n=$#
	while [ "$cg_n" -gt 0 ]; do
		case "$1" in
			--settings) cg_made="$cg_made --settings $(crun_quote "$2")"; shift 2; cg_n=$((cg_n - 2)); continue ;;
			--settings=*) cg_made="$cg_made --settings $(crun_quote "${1#--settings=}")"; shift; cg_n=$((cg_n - 1)); continue ;;
			--firmware) cg_made="$cg_made --firmware $(crun_quote "$2")"; set -- "$@" "$1" "$2"; shift 2; cg_n=$((cg_n - 2)); continue ;;
			--firmware=*) cg_made="$cg_made --firmware $(crun_quote "${1#--firmware=}")"; set -- "$@" "$1"; shift; cg_n=$((cg_n - 1)); continue ;;
		esac
		set -- "$@" "$1"; shift; cg_n=$((cg_n - 1))
	done
	cg_dir="$(mktemp -d "$work/project.XXXXXX")"
	eval "\"\$engine\" --new-project \"\$cg_dir/game.chimeraProject\" \"\$cg_pkg\" \"\$cg_game\" \"\$cg_movie\" $cg_made" \
		> "$cg_dir/make.log" 2>&1 || { echo "chimera-run: no project made: $(tail -1 "$cg_dir/make.log")" >&2; return 2; }
	"$engine" --project "$cg_dir/game.chimeraProject" "$cg_pkg" --files "$(dirname "$cg_game")" "$@"
}
chimera_root="${CHIMERA_ROOT:-$HOME/chimera}"
engine="$chimera_root/build/meson-linux/chimera-run"
pkg="$root/build/package/eden.chimeraCore"
content="${EDEN_CONTENT:-$root/build/content}"
# llvmpipe gives OpenGL 4.5 and Eden asks for 4.6, which Mesa can be told to say
export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460
unset DISPLAY

# <what> <log> <command...>: a build step; on failure its log's end is said
step() {
	what="$1"; log="$2"; shift 2
	mkdir -p "$root/build"
	"$@" >"$log" 2>&1 && return 0
	echo "FAIL $what (${log#$root/})"
	tail -40 "$log" | sed 's/^/  | /'
	exit 1
}
if [ "${1:-}" != "-n" ]; then
	step "the native reference did not build" "$root/build/native-build.log" sh "$here/build-native.sh"
	step "the guest did not build" "$root/build/guest-build.log" sh "$here/build-guest.sh"
	step "core.wbx did not link" "$root/build/core-link.log" sh "$here/build-core.sh"
	step "the package did not build" "$root/build/package.log" sh "$here/build-package.sh" -n -r "$chimera_root"
	echo "PASS the package builds: $(grep -o 'package sha1 [0-9a-f]*' "$root/build/package.log")"
fi
mkdir -p "$work"

fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
skip() { echo "SKIP $*"; }

# ---- without content ----

# the series is the tree: apply-patches.sh judges the whole of it
if out=$(sh "$here/apply-patches.sh" 2>&1) && echo "$out" | grep -q "already applied"; then
	pass "the patches are the tree: $(echo "$out" | tail -1)"
else
	fail "the patch series and extern/eden disagree - $(echo "$out" | tail -1)"
fi

# what the frontend reads of the package, against what the driver takes
verdict=$(python3 - "$here" <<'PYEOF'
import json, re, sys
here = sys.argv[1]
cfg = json.load(open(here + "/waterbox.config"))
slots = json.load(open(here + "/file_slots.json"))
json.load(open(here + "/default_keybinds.json"))
lic = json.load(open(here + "/package-licenses.json"))
buttons, axes = cfg["input"]["buttons"], cfg["input"]["axes"]
harness = open(here + "/gate-harness.h").read()
n_axes = int(re.search(r"#define HARNESS_AXES (\d+)", harness).group(1))
problems = []
if len(buttons) != 33: problems.append("%d buttons, the driver takes 33" % len(buttons))
if len(axes) != n_axes: problems.append("%d axes, the driver takes %d" % (len(axes), n_axes))
for a in axes:
    if "Touch" in a["name"] and (a["min"], a["max"], a["neutral"]) != (0, 65535, 32768):
        problems.append(a["name"] + " is not 0..65535 around 32768")
ids = [s["id"] for s in slots["slots"]]
if ids != ["game", "savedata", "content"]: problems.append("slots are %s" % ids)
content = slots["slots"][-1]
if content.get("formats") != ["nsp", "nsz"] or content.get("min") != 0: problems.append("the content slot takes %s" % content.get("formats"))
# the resolutions the setting offers are the ones the driver knows, and the
# declared picture holds the largest of them (docked, 1920x1080, at 3x)
driver = open(here + "/driver/eden_driver.cpp").read()
known = re.findall(r'\{ "([0-9.]+x)", Settings::ResolutionSetup::', driver)
res = [x for x in cfg["settings"] if x["name"] == "resolution"]
if not res or res[0]["options"] != known or res[0]["default"] != "1x":
    problems.append("the resolution setting offers %s, the driver knows %s" % (res[0]["options"] if res else None, known))
elif (cfg["video"]["width"], cfg["video"]["height"]) != (1920 * 3, 1080 * 3):
    problems.append("the picture is declared %dx%d, docked at 3x is 5760x3240" % (cfg["video"]["width"], cfg["video"]["height"]))
if lic["effectiveTerms"] != "GPL-3.0-or-later": problems.append("the package's terms")
if problems:
    print("; ".join(problems)); sys.exit(1)
print("%d buttons, %d axes, slots %s, resolutions %s, %d licensed components" % (len(buttons), len(axes), "+".join(ids), "/".join(known), len(lic["components"])))
PYEOF
)
if [ $? = 0 ]; then pass "the package's declarations: $verdict"; else fail "the package's declarations: $verdict"; fi

# Deleting a file kept in the core's in-memory file system. The C library's
# remove() makes the unlink system call itself instead of calling unlink(), so
# before memfs had its own remove() a file a game deleted was asked of the
# sandbox, which has no such call and stopped the core (a game replacing its
# save). The test is built and run natively, where the same remove() goes to
# the host and fails; it needs only a C++ compiler.
if g++ -std=c++20 -O1 -w -o "$work/memfs-remove" "$here/tests/memfs-remove.cpp" "$here/driver/memfs.cpp" -ldl 2>"$work/memfs-remove.err"; then
	if out=$("$work/memfs-remove" 2>&1); then
		pass "a file deleted with remove() or std::filesystem::remove leaves the in-memory file system"
	else
		fail "deleting from the in-memory file system - $out"
	fi
else
	fail "the delete test did not compile - $(head -1 "$work/memfs-remove.err")"
fi

# The texts a person reads are keys of languages/en.json, in every language
# Chimera has. Proper nouns stay as they are written in all of them: Eden,
# Switch, OpenGL and the rest are never translated or given endings.
if out=$(python3 -I "$here/tests/check-names.py" "$here/languages" 2>&1); then
	pass "names: $(printf '%s' "$out" | tail -1)"
else
	fail "names: $(printf '%s' "$out" | grep -v 'bent or missing names in' | head -3 | tr '\n' ' ')"
fi
# Chimera's own check of a core's texts: every key named, every name a key.
# (That the names stay as they are - Chimera's, this core's - is the
# engine's check of the package's tables, which the contract tests run.) It
# reads waterbox/, where this core's code and tables are, and not the whole
# checkout: CI puts Chimera's own checkout inside it, and Chimera's sources
# hold keys of their own.
ct="$chimera_root/tools/check-core-texts.py"
if [ -f "$ct" ]; then
	if ct_out=$(python3 -I "$ct" "$here" 2>&1); then
		pass "texts: $(printf '%s' "$ct_out" | tail -1)"
	else
		fail "texts: $(printf '%s' "${ct_out:-}" | head -3 | tr '\n' ' ')"
	fi
else
	skip "texts: no Chimera checkout with tools/check-core-texts.py at $chimera_root"
fi

# The core's own tool, Decompress NSZ (Chimera's Tools > Core-Specific
# Tools), as the window runs it: in Chimera's sandbox, through chimera-run
# --run-tool, with no game. A small .nsz made here (tests/make-nsz.py: made-up
# bytes, encrypted and compressed as a real one is), one compressed as one
# stream and one in blocks, must each come back as the .nsp it stands for,
# file for file; a file that is no NSZ is refused by name and leaves no file.
if [ -x "$engine" ] && [ -f "$pkg" ]; then
	tw="$work/tool"
	rm -rf "$tw"; mkdir -p "$tw"
	python3 "$here/tests/make-nsz.py" "$tw" > "$tw/make.log" 2>&1
	python3 "$here/tests/nsp-entries.py" "$tw/expected.nsp" > "$tw/expected.list" 2>&1
	ok=1; said=""
	for kind in solid block; do
		"$engine" --run-tool decompress_nsz "$pkg" --input input="$tw/$kind.nsz" --output output="$tw/$kind.nsp" > "$tw/$kind.out" 2>&1
		python3 "$here/tests/nsp-entries.py" "$tw/$kind.nsp" > "$tw/$kind.list" 2>&1
		if ! grep -q '^{"done": ' "$tw/$kind.out" || ! cmp -s "$tw/$kind.list" "$tw/expected.list"; then ok=0; said="$said $kind: $(head -c 160 "$tw/$kind.out")"; fi
	done
	cp "$tw/expected.list" "$tw/not-an-nsz.nsz"
	"$engine" --run-tool decompress_nsz "$pkg" --input input="$tw/not-an-nsz.nsz" --output output="$tw/refused.nsp" > "$tw/refused.out" 2>&1
	if [ "$ok" = 1 ] && grep -q '^{"refused": .*not-an-nsz.nsz' "$tw/refused.out" && [ ! -e "$tw/refused.nsp" ]; then
		pass "Decompress NSZ: a solid and a block .nsz made here come back as their .nsp ($(head -1 "$tw/make.log")); a file that is no NSZ is refused by name, no file left"
	else
		fail "Decompress NSZ:$said refused: $(head -c 160 "$tw/refused.out")"
	fi
else
	skip "Decompress NSZ: no $engine or no build/package/eden.chimeraCore"
fi

# ---- with content ----

game="$(ls "$content"/*.nsp "$content"/*.xci 2>/dev/null | head -1)"
firmware="$(ls "$content"/Firmware*.zip "$content"/firmware*.zip 2>/dev/null | head -1)"
if [ -z "$game" ] || [ ! -f "$content/prod.keys" ]; then
	skip "every leg that runs a game: no .nsp/.xci or no prod.keys in ${content#$root/}"
	echo "$fails failed"
	exit "$fails"
fi
if [ ! -x "$run" ] || [ ! -x "$wbx" ] || [ ! -f "$core" ]; then
	fail "the runners are not built (build/native, build/wbx)"
	echo "$fails failed"
	exit "$fails"
fi
fw="--firmware prod.keys=$content/prod.keys"
[ -f "$content/title.keys" ] && fw="$fw --firmware title.keys=$content/title.keys"
have_fw=0
[ -n "$firmware" ] && have_fw=1

# <name> <frames> [options...]: a run's lines in $work/<name>.out
native() {
	name="$1"; frames="$2"; shift 2
	if [ "$have_fw" = 1 ]; then set -- --firmware "firmware.zip=$firmware" "$@"; fi
	"$run" "$game" --work "$work/$name-work" $fw --frames "$frames" --timeout 1800 --digest-every 1 "$@" \
		>"$work/$name.out" 2>"$work/$name.err"
	echo $?
}
sandboxed() {
	name="$1"; frames="$2"; shift 2
	if [ "$have_fw" = 1 ]; then set -- --firmware "firmware.zip=$firmware" "$@"; fi
	"$wbx" "$core" "$game" --work "$work/$name-work" $fw --frames "$frames" --timeout 3600 --digest-every 1 "$@" \
		>"$work/$name.out" 2>"$work/$name.err"
	echo $?
}
pictures() { grep -o 'video=[0-9a-f]*' "$work/$1.out" | sort -u | wc -l; }
# the runner's own lines of a run (the guest's allocator says what it set up
# on the same stdout in the sandbox, and nothing natively)
harness_lines() { grep -E '^(frame |app=|loaded frame )' "$work/$1.out" > "$work/$1.lines"; }
frames_of() { grep '^frame ' "$work/$1.out" > "$work/$1.frames"; }
first_diff() { diff "$work/$1" "$work/$2" | sed -n 2p | cut -c1-120; }

# The script: A at 300 and 500 (the game's first screens), which the legs
# below share. With no input the game waits on its first dialog for good.
script="$here/tests/press-a.txt"
N=400

# 1. two native runs, then the sandbox: every line, the memory digest among them
ra=$(native n-a $N --input "$script" --ram-every 50)
rb=$(native n-b $N --input "$script" --ram-every 50)
if [ "$ra" != 0 ] || [ "$rb" != 0 ]; then
	fail "a native run failed (exit $ra, $rb; build/gate/n-a.err)"
elif ! cmp -s "$work/n-a.out" "$work/n-b.out"; then
	fail "two native runs differ - $(first_diff n-a.out n-b.out)"
else
	pass "two native runs the same in all $N frames: $(pictures n-a) pictures, $(grep -c 'ram=' "$work/n-a.out") memory digests"
fi
rw=$(sandboxed w-a $N --input "$script" --ram-every 50)
if [ "$rw" != 0 ]; then
	fail "the sandboxed run failed (exit $rw; build/gate/w-a.err)"
elif harness_lines n-a; harness_lines w-a; ! cmp -s "$work/n-a.lines" "$work/w-a.lines"; then
	fail "native and sandbox differ - $(first_diff n-a.lines w-a.lines)"
else
	pass "native == sandbox in all $N frames (clock, CPU ticks, picture, sound, memory)"
fi

# 2. the comparison can fail, and input reaches the game: without the script
# the run is another from the frame the button goes down
r=$(native n-idle $N --ram-every 50)
frames_of n-a; frames_of n-idle
parted=$(diff "$work/n-a.frames" "$work/n-idle.frames" | grep '^<' | head -1 | awk '{print $3}')
if [ "$r" != 0 ]; then
	fail "the run with no input failed (exit $r)"
elif [ -z "$parted" ]; then
	fail "a run with no input is the same as one with the script: the comparison sees nothing, or input does not arrive"
elif [ "$parted" -lt 300 ] || [ "$parted" -gt 330 ]; then
	fail "the run with no input parts at frame $parted, and A goes down at 300"
else
	pass "input arrives: with no script the run parts from the scripted one at frame $parted (A goes down at 300)"
fi

# 3. no picture: the Null renderer is the same machine in both flavours
rn=$(native n-null $N --input "$script" --renderer null --ram-every 50)
rw=$(sandboxed w-null $N --input "$script" --renderer null --ram-every 50)
if [ "$rn" != 0 ] || [ "$rw" != 0 ]; then
	fail "a Null renderer run failed (exit $rn, $rw)"
elif harness_lines n-null; harness_lines w-null; ! cmp -s "$work/n-null.lines" "$work/w-null.lines"; then
	fail "Null renderer: native and sandbox differ - $(first_diff n-null.lines w-null.lines)"
else
	pass "Null renderer: native == sandbox in all $N frames"
fi

# 4. a renderer made again before every frame draws what one left alone draws
r=$(native n-rebuild $N --input "$script" --ram-every 50 --rebuild-every-frame)
if [ "$r" != 0 ]; then
	fail "the run that rebuilds its renderer failed (exit $r; build/gate/n-rebuild.err)"
elif ! cmp -s "$work/n-a.out" "$work/n-rebuild.out"; then
	fail "a renderer made again draws another run - $(first_diff n-a.out n-rebuild.out)"
else
	pass "a renderer made again before every frame: the same $N frames, $(pictures n-rebuild) pictures, memory included"
fi
# ... and that leg can fail: with what the GPU drew left on the GPU, the
# renderer made again has lost it
r=$(native n-rebuild-lossy 120 --input "$script" --rebuild-every-frame --fast-states)
head -120 "$work/n-a.frames" | sed 's/ ram=.*//' > "$work/n-a.120"
grep '^frame ' "$work/n-rebuild-lossy.out" > "$work/n-rebuild-lossy.frames"
if [ "$r" = 0 ] && ! cmp -s "$work/n-a.120" "$work/n-rebuild-lossy.frames"; then
	pass "negative control: without the frame-end write-back (Pictures in States: fast) a rebuilt renderer draws wrong ($(diff "$work/n-a.120" "$work/n-rebuild-lossy.frames" | grep -c '^>') of 120 frames differ)"
else
	fail "negative control: the rebuild leg passed with the write-back off (exit $r) - it proves nothing"
fi
# ... while a run that loads nothing is the same run with it off: the setting
# is what a state holds of the picture, and nothing of the machine
r=$(native n-fast $N --input "$script" --fast-states)
frames_of n-fast
sed 's/ ram=.*//' "$work/n-a.frames" > "$work/n-a.noram"
if [ "$r" != 0 ]; then
	fail "the run with Pictures in States: fast failed (exit $r)"
elif ! cmp -s "$work/n-a.noram" "$work/n-fast.frames"; then
	fail "Pictures in States: fast changes the run itself - $(first_diff n-a.noram n-fast.frames)"
else
	pass "Pictures in States: fast is the same run when nothing is loaded - $N frames, the clock, the CPU's ticks, $(pictures n-fast) pictures, the sound"
fi

# 5. states, in the sandbox: one saved and loaded before every frame, and one
# saved by a process and loaded by another
S=200; SPLIT=120
r=$(sandboxed w-plain $S --input "$script" --ram-every 40)
frames_of w-plain
r2=$(sandboxed w-rerecord $S --input "$script" --ram-every 40 --rerecord)
frames_of w-rerecord
if [ "$r" != 0 ] || [ "$r2" != 0 ]; then
	fail "a state run failed (exit $r, $r2; build/gate/w-rerecord.err)"
elif ! cmp -s "$work/w-plain.frames" "$work/w-rerecord.frames"; then
	fail "a state saved and loaded before every frame changes the run - $(first_diff w-plain.frames w-rerecord.frames)"
else
	pass "a state saved and loaded before every frame: the same $S frames, $(pictures w-rerecord) pictures, memory included"
fi
r=$(sandboxed w-first $SPLIT --input "$script" --save-state "$work/gate.state")
r2=$(sandboxed w-second $((S - SPLIT)) --input "$script" --ram-every 40 --state "$work/gate.state")
frames_of w-second
awk -v s="$SPLIT" '$2 > s' "$work/w-plain.frames" > "$work/w-plain.tail"
size=$(wc -c < "$work/gate.state" 2>/dev/null || echo 0)
rm -f "$work/gate.state"
if [ "$r" != 0 ] || [ "$r2" != 0 ]; then
	fail "the state did not cross processes (exit $r, $r2; build/gate/w-second.err)"
elif ! cmp -s "$work/w-plain.tail" "$work/w-second.frames"; then
	fail "a state loaded in another process runs on differently - $(first_diff w-plain.tail w-second.frames)"
else
	pass "a state saved at frame $SPLIT ($((size / 1048576)) MiB) and loaded by another process: the next $((S - SPLIT)) frames the same, memory included"
fi

# 6. turbo: frames not read back are still frames - the machine is the same after
r=$(sandboxed w-turbo $S --input "$script" --ram-every 40 --turbo 20:150)
frames_of w-turbo
awk '$2 > 150' "$work/w-plain.frames" > "$work/w-plain.after"
awk '$2 > 150' "$work/w-turbo.frames" > "$work/w-turbo.after"
if [ "$r" != 0 ]; then
	fail "the turbo run failed (exit $r)"
elif ! cmp -s "$work/w-plain.after" "$work/w-turbo.after"; then
	fail "after turbo the run differs - $(first_diff w-plain.after w-turbo.after)"
else
	pass "turbo over frames 20..150: the $(wc -l < "$work/w-turbo.after") frames after are the same, picture and memory"
fi

# 7. save data: out through the export, back in through the slot
rm -rf "$work/sd-out" "$work/sd-again"
r=$(sandboxed w-save 600 --input "$script" --savedata-out "$work/sd-out")
files=$(cd "$work/sd-out" 2>/dev/null && find . -type f | wc -l)
if [ "$r" != 0 ]; then
	fail "the run that exports save data failed (exit $r)"
elif [ "${files:-0}" = 0 ]; then
	skip "save data: the game saved nothing in 600 frames, so there is nothing to carry"
else
	rm -f "$work/sd.zip"
	python3 -c 'import shutil, sys; shutil.make_archive(sys.argv[1], "zip", sys.argv[2])' "$work/sd" "$work/sd-out"
	r=$(sandboxed w-saved 600 --input "$script" --savedata-in "$work/sd.zip" --savedata-out "$work/sd-again")
	frames_of w-save; frames_of w-saved
	missing=$(cd "$work/sd-out" && find . -type f | while read -r f; do [ -f "$work/sd-again/$f" ] || echo "$f"; done | head -1)
	if [ "$r" != 0 ]; then
		fail "the run that takes save data failed (exit $r; build/gate/w-saved.err)"
	elif [ -n "$missing" ]; then
		fail "save data put in did not come out again: $missing"
	elif cmp -s "$work/w-save.frames" "$work/w-saved.frames"; then
		fail "a game given its save data ran exactly as one with none: it did not read it"
	else
		pass "save data: $files files out ($(cd "$work/sd-out" && find . -type f | head -1 | cut -c3-)), taken back by a new machine, which runs differently from frame $(diff "$work/w-save.frames" "$work/w-saved.frames" | grep '^<' | head -1 | awk '{print $3}') and exports them again"
	fi
fi

# 7b. the touch screen: a finger put down by the script is where Eden's
# emulated console says it is (a quarter across, three quarters down), for
# the frames the script holds it, and nowhere before or after
printf '20 0x100000000 0 0 0 0 0 0 0 0 16384 49152\n30 0 0 0 0 0 0 0 0 0 32768 32768\n' > "$work/touch.txt"
r=$(CHIMERA_EDEN_TOUCH_TRACE=1 native n-touch 40 --input "$work/touch.txt" --renderer null)
verdict=$(python3 - "$work/n-touch.err" <<'PYEOF'
import re, sys
seen = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"touch frame=(\d+) pressed=(\d) x=([\d.]+) y=([\d.]+)", line)
    if m:
        seen[int(m.group(1))] = (int(m.group(2)), float(m.group(3)), float(m.group(4)))
if len(seen) < 40:
    print("only %d frames traced" % len(seen)); sys.exit(1)
for f in range(1, 41):
    pressed, x, y = seen[f]
    want = 20 <= f < 30
    if bool(pressed) != want:
        print("frame %d: pressed=%d, the script says %d" % (f, pressed, want)); sys.exit(1)
    if want and (abs(x - 16384 / 65536) > 1e-4 or abs(y - 49152 / 65536) > 1e-4):
        print("frame %d: at %.5f,%.5f, the script says 0.25,0.75" % (f, x, y)); sys.exit(1)
print("down in frames 20..29 at 0.25,0.75 and up in the other 30")
PYEOF
)
vr=$?
if [ "$r" = 0 ] && [ "$vr" = 0 ]; then
	pass "touch: $verdict"
else
	fail "touch (exit $r): $verdict"
fi

# 8. a machine with no keys says so
"$run" "$game" --work "$work/nokeys-work" --frames 1 --timeout 60 >"$work/nokeys.out" 2>"$work/nokeys.err"
r=$?
if [ "$r" != 0 ] && grep -aq "Init failed: error.no_prod_keys" "$work/nokeys.err"; then
	pass "with no prod.keys the core refuses to start, saying the keys are missing (exit $r)"
else
	fail "with no prod.keys the core did not refuse cleanly (exit $r) - $(tail -1 "$work/nokeys.err")"
fi

# 8a. Updates and DLC: what is put in the "content" slot must be an update or
# DLC. The game itself there, or a file that is no .nsp at all, is refused
# with its name rather than ignored.
head -c 100000 /dev/urandom > "$work/junk.nsp"
"$run" "$game" --work "$work/content-base-work" $fw --frames 3 --renderer null --content "$game" >"$work/content-base.out" 2>"$work/content-base.err"
rb=$?
"$run" "$game" --work "$work/content-junk-work" $fw --frames 3 --renderer null --content "$work/junk.nsp" >"$work/content-junk.out" 2>"$work/content-junk.err"
rj=$?
if [ "$rb" != 0 ] && grep -aq "error\.not_content.$(basename "$game" | sed 's/[][]/./g')" "$work/content-base.err" \
	&& [ "$rj" != 0 ] && grep -aq "error\.not_content.junk\.nsp" "$work/content-junk.err"; then
	pass "updates and DLC: the game itself in that slot is refused, and so is a file that is no .nsp, each by name"
else
	fail "updates and DLC: a wrong file in that slot was not refused by name (exit $rb, $rj) - $(grep 'Init failed' "$work/content-base.err" "$work/content-junk.err" | head -1 | cut -c1-140)"
fi

# 8b. Internal Resolution above 1x. The renderer draws at the larger size and
# the console's memory gets each picture scaled down, so a renderer made again
# after a load must get the pictures it had back from the state
# (driver/kept_images.cpp, patch 0009). Natively the rebuild before every
# frame stands for a load there: told first, as the engine tells the core
# before every state, it must draw the run of a renderer left alone; untold it
# must not (the negative control). Then the sandbox, where the states are real.
R=2x
ra=$(native n-$R $N --input "$script" --ram-every 50 --resolution $R)
size=$(tail -1 "$work/n-$R.out" | grep -o '[0-9]*x[0-9]*$')
rb=$(native n-$R-rebuild $N --input "$script" --ram-every 50 --resolution $R --rebuild-every-frame)
rc=$(CHIMERA_EDEN_NO_KEEP=1 native n-$R-untold $N --input "$script" --ram-every 50 --resolution $R --rebuild-every-frame)
frames_of n-$R; frames_of n-$R-untold
if [ "$ra" != 0 ] || [ "$rb" != 0 ] || [ "$rc" != 0 ]; then
	fail "a run at $R failed (exit $ra, $rb, $rc; build/gate/n-$R*.err)"
elif [ "$size" != "2560x1440" ]; then
	fail "at $R the picture is $size, not 2560x1440"
elif ! cmp -s "$work/n-$R.out" "$work/n-$R-rebuild.out"; then
	fail "at $R a renderer made again draws another run - $(first_diff n-$R.out n-$R-rebuild.out)"
elif cmp -s "$work/n-$R.frames" "$work/n-$R-untold.frames"; then
	fail "negative control: at $R a renderer made again untold draws the same run - the leg proves nothing"
else
	pass "at $R ($size) a renderer made again before every frame, told first: the same $N frames, memory included; untold, $(diff "$work/n-$R.frames" "$work/n-$R-untold.frames" | grep -c '^>') frames differ from frame $(diff "$work/n-$R.frames" "$work/n-$R-untold.frames" | grep '^<' | head -1 | awk '{print $3}')"
fi
rw=$(sandboxed w-$R $S --input "$script" --ram-every 50 --resolution $R)
frames_of w-$R
r=$(sandboxed w-$R-rerecord $S --input "$script" --ram-every 50 --resolution $R --rerecord)
frames_of w-$R-rerecord
r2=$(RUN_WBX_NO_STATE_SAVING=1 sandboxed w-$R-untold $S --input "$script" --ram-every 50 --resolution $R --rerecord)
frames_of w-$R-untold
head -$S "$work/n-$R.frames" > "$work/n-$R.head"
if [ "$rw" != 0 ] || [ "$r" != 0 ] || [ "$r2" != 0 ]; then
	fail "a sandboxed run at $R failed (exit $rw, $r, $r2; build/gate/w-$R*.err)"
elif ! cmp -s "$work/n-$R.head" "$work/w-$R.frames"; then
	fail "at $R native and sandbox differ - $(first_diff n-$R.head w-$R.frames)"
elif ! cmp -s "$work/w-$R.frames" "$work/w-$R-rerecord.frames"; then
	fail "at $R a state saved and loaded before every frame changes the run - $(first_diff w-$R.frames w-$R-rerecord.frames)"
elif cmp -s "$work/w-$R.frames" "$work/w-$R-untold.frames"; then
	fail "negative control: at $R states taken untold (no StateSaving) give the same run - the leg proves nothing"
else
	pass "at $R native == sandbox in $S frames, and in the sandbox a state saved and loaded before every frame: the same $S frames, memory included; with the core untold of each state, $(diff "$work/w-$R.frames" "$work/w-$R-untold.frames" | grep -c '^>') frames differ"
fi
r=$(sandboxed w-$R-first $SPLIT --input "$script" --resolution $R --save-state "$work/gate-$R.state")
r2=$(sandboxed w-$R-second $((S - SPLIT)) --input "$script" --ram-every 50 --resolution $R --state "$work/gate-$R.state")
frames_of w-$R-second
awk -v s="$SPLIT" '$2 > s' "$work/w-$R.frames" > "$work/w-$R.tail"
size=$(wc -c < "$work/gate-$R.state" 2>/dev/null || echo 0)
rm -f "$work/gate-$R.state"
if [ "$r" != 0 ] || [ "$r2" != 0 ]; then
	fail "at $R the state did not cross processes (exit $r, $r2; build/gate/w-$R-second.err)"
elif ! cmp -s "$work/w-$R.tail" "$work/w-$R-second.frames"; then
	fail "at $R a state loaded in another process runs on differently - $(first_diff w-$R.tail w-$R-second.frames)"
else
	pass "at $R a state saved at frame $SPLIT ($((size / 1048576)) MiB) and loaded by another process: the next $((S - SPLIT)) frames the same, memory included"
fi
# every other resolution starts and draws at its size, and differs from 1x
for R in 1.5x 3x; do
	r=$(native n-$R 120 --input "$script" --resolution $R)
	size=$(tail -1 "$work/n-$R.out" | grep -o '[0-9]*x[0-9]*$')
	case $R in 1.5x) want=1920x1080 ;; 3x) want=3840x2160 ;; esac
	if [ "$r" != 0 ]; then
		fail "the run at $R failed (exit $r; build/gate/n-$R.err)"
	elif [ "$size" != "$want" ]; then
		fail "at $R the picture is $size, not $want"
	else
		pass "at $R the run starts and draws at $size"
	fi
done
r=$(native n-docked-2x 120 --input "$script" --resolution 2x --docked)
size=$(tail -1 "$work/n-docked-2x.out" | grep -o '[0-9]*x[0-9]*$')
if [ "$r" = 0 ] && [ "$size" = "3840x2160" ]; then
	pass "docked at 2x the picture is $size"
else
	fail "docked at 2x (exit $r) the picture is $size, not 3840x2160"
fi

# 9. the package through Chimera's own engine, headless
if [ -x "$engine" ] && [ -f "$pkg" ]; then
	python3 "$here/tests/make-movie.py" "$script" 330 "$work/engine.movie"
	set -- --firmware "prod.keys=$content/prod.keys"
	[ -f "$content/title.keys" ] && set -- "$@" --firmware "title.keys=$content/title.keys"
	[ "$have_fw" = 1 ] && set -- "$@" --firmware "firmware.zip=$firmware"
	crun_game "$pkg" "$game" "$work/engine.movie" --gpu "$@" --screenshot "330=$work/engine-330.png" >"$work/engine.out" 2>&1
	r=$?
	if [ "$r" = 0 ] && grep -q '^frames=330' "$work/engine.out"; then
		pass "the package in chimera-run: 330 frames of a movie - $(grep -o 'frames=330.*' "$work/engine.out" | cut -c1-100)"
	else
		fail "the package in chimera-run (exit $r) - $(tail -2 "$work/engine.out" | head -1 | cut -c1-140); see build/gate/engine.out"
	fi
else
	skip "the package in chimera-run: no $engine or no build/package/eden.chimeraCore"
fi

# 9b. A project, as a person's: the game in its slot under its own name, the
# firmware pinned, Internal Resolution 2x, through chimera-run --project. A
# state saved and loaded by the engine around every frame (which tells the
# core before each, StateSaving) shows the same pictures as the straight run;
# with the engine told not to tell it (CHIMERA_NO_STATE_SAVING), it does not.
if [ -x "$engine" ] && [ -f "$pkg" ]; then
	ep="$work/project"; rm -rf "$ep"; mkdir -p "$ep"
	set -- --firmware "prod.keys=$content/prod.keys"
	tk=""; fwp=""
	[ -f "$content/title.keys" ] && set -- "$@" --firmware "title.keys=$content/title.keys" && tk="--firmware=title.keys=$content/title.keys"
	[ "$have_fw" = 1 ] && set -- "$@" --firmware "firmware.zip=$firmware" && fwp="--firmware=firmware.zip=$firmware"
	python3 "$here/tests/make-project.py" "$pkg" "$ep/p.chimeraProject" "$script" 160 --set=resolution=2x \
		"--firmware=prod.keys=$content/prod.keys" ${tk:+"$tk"} ${fwp:+"$fwp"} "game=$game" > "$ep/make.log" 2>&1
	for v in straight rerecord untold; do
		extra=""; [ $v != straight ] && extra=--rerecord
		untold=""; [ $v = untold ] && untold=1
		env ${untold:+CHIMERA_NO_STATE_SAVING=1} "$engine" --project "$ep/p.chimeraProject" "$pkg" --files "$content" \
			--allow-core-mismatch --gpu "$@" $extra --screenshot "75=$ep/$v-75.tga" --screenshot "150=$ep/$v-150.tga" \
			> "$ep/$v.log" 2>&1
		echo $? > "$ep/$v.exit"
	done
	if [ "$(cat "$ep/straight.exit" "$ep/rerecord.exit" "$ep/untold.exit" | tr -d '\n')" != 000 ] || ! grep -q '^frames=160' "$ep/straight.log"; then
		fail "a project at 2x in chimera-run failed (exits $(cat "$ep"/*.exit | tr '\n' ' ')) - $(grep -v 'heap' "$ep/straight.log" | tail -1 | cut -c1-140)"
	elif ! cmp -s "$ep/straight-75.tga" "$ep/rerecord-75.tga" || ! cmp -s "$ep/straight-150.tga" "$ep/rerecord-150.tga"; then
		fail "a project at 2x: with a state saved and loaded around every frame, the pictures at 75 or 150 differ"
	elif cmp -s "$ep/straight-75.tga" "$ep/untold-75.tga" && cmp -s "$ep/straight-150.tga" "$ep/untold-150.tga"; then
		fail "negative control: a project at 2x with the core untold of each state shows the same pictures - the leg proves nothing"
	else
		pass "a project at 2x through chimera-run: a state around every frame shows the pictures of the straight run at frames 75 and 150; untold, they differ"
	fi
else
	skip "a project in chimera-run: no $engine or no build/package/eden.chimeraCore"
fi

# 10. Games that once failed, each in a folder of its own under
# build/test-games (or EDEN_TEST_GAMES), run in the sandbox with the keys and
# system software above. A test whose folder holds no game SKIPs.
games="${EDEN_TEST_GAMES:-$root/build/test-games}"
# <folder>: the game in that folder, or nothing
game_in() { ls "$games/$1"/*.nsp "$games/$1"/*.xci 2>/dev/null | head -1; }
# <folder> <frames> [options...]: like sandboxed, with that folder's game
sandboxed_game() {
	name="$1"; frames="$2"; shift 2
	g="$(game_in "$name")"
	if [ "$have_fw" = 1 ]; then set -- --firmware "firmware.zip=$firmware" "$@"; fi
	"$wbx" "$core" "$g" --work "$work/$name-work" $fw --frames "$frames" --digest-every 100 "$@" \
		>"$work/$name.out" 2>"$work/$name.err"
	echo $?
}

# 10a. viewport-off: a game whose last draw of a frame, the copy of its
# picture onto the screen, turns the viewport off (its positions are then
# pixels) and draws in an area far larger than the screen. The OpenGL renderer
# still flipped that draw by the scale an earlier draw had left in the
# viewport, onto rows far below the screen, and the screen stayed black
# (patch 0011). MARVEL Cosmic Invasion does this from its first logo on; at
# frame 1000 it shows its studio's logo on white.
if [ -n "$(game_in viewport-off)" ]; then
	r=$(sandboxed_game viewport-off 1100 --timeout 1800 --screenshot "1000=$work/viewport-off-1000.tga")
	lit=$(python3 "$here/tests/lit-pixels.py" "$work/viewport-off-1000.tga" 2>/dev/null || echo 0)
	if [ "$r" = 0 ] && [ "$lit" -gt 100000 ]; then
		pass "viewport-off: the picture at frame 1000 has $lit pixels that are not black"
	else
		fail "viewport-off (exit $r): the picture at frame 1000 has $lit pixels that are not black (more than 100000 expected); see build/gate/viewport-off.err"
	fi
else
	skip "viewport-off: no game in ${games#$root/}/viewport-off"
fi

# 10b. core-wait: a game that pauses one of its threads while that thread is
# running on another of the console's four cores. Here the four cores take
# turns on one thread, so the core asking waited for ever for the other to
# run, and no frame ended (patch 0010). PAC-MAN CHAMPIONSHIP EDITION 2 PLUS
# does it at start; from frame 550 to 650 it shows the developer's name on
# black (from 500, before reads took time: see 10e).
if [ -n "$(game_in core-wait)" ]; then
	r=$(sandboxed_game core-wait 950 --timeout 600 --screenshot "600=$work/core-wait-600.tga")
	lit=$(python3 "$here/tests/lit-pixels.py" "$work/core-wait-600.tga" 2>/dev/null || echo 0)
	if [ "$r" = 0 ] && grep -q '^app=.* frames=950 ' "$work/core-wait.out" && [ "$lit" -gt 5000 ]; then
		pass "core-wait: 950 frames, and the picture at frame 600 has $lit pixels that are not black"
	else
		fail "core-wait (exit $r): $(grep -c '^frame ' "$work/core-wait.out") frames reported, $lit pixels that are not black at frame 600; see build/gate/core-wait.err"
	fi
else
	skip "core-wait: no game in ${games#$root/}/core-wait"
fi

# 10c. decompress-nsz: a real .nsz (a game, update or DLC compressed as one
# stream) and the .nsp it was made from, in build/test-games/decompress-nsz:
# the tool gives the same files; and the .nsz played is refused by its name,
# saying what to do (Eden would say only that the game holds no program).
nsz="$(ls "$games/decompress-nsz"/*.nsz 2>/dev/null | head -1)"
plain="$(ls "$games/decompress-nsz"/*.nsp 2>/dev/null | head -1)"
if [ -n "$nsz" ] && [ -n "$plain" ] && [ -x "$engine" ] && [ -f "$pkg" ]; then
	dw="$work/decompress-nsz"
	rm -rf "$dw"; mkdir -p "$dw"
	"$engine" --run-tool decompress_nsz "$pkg" --input input="$nsz" --output output="$dw/out.nsp" > "$dw/tool.out" 2>&1
	python3 "$here/tests/nsp-entries.py" "$dw/out.nsp" > "$dw/out.list" 2>&1
	python3 "$here/tests/nsp-entries.py" "$plain" > "$dw/plain.list" 2>&1
	"$run" "$nsz" --work "$dw/play" $fw --frames 1 --renderer null > "$dw/play.out" 2>&1; r=$?
	if grep -q '^{"done": ' "$dw/tool.out" && cmp -s "$dw/out.list" "$dw/plain.list" \
		&& [ "$r" != 0 ] && grep -q "error.solid_nsz.$(basename "$nsz" | sed 's/[][]/./g')" "$dw/play.out"; then
		pass "decompress-nsz: $(basename "$nsz") comes back as the .nsp it was made from ($(wc -l < "$dw/plain.list") files; $(grep '^{' "$dw/tool.out" | cut -c1-80)); played, it is refused by its name"
	else
		fail "decompress-nsz (play exit $r): $(head -c 160 "$dw/tool.out"); $(grep -m1 'Init failed' "$dw/play.out" | cut -c1-120)"
	fi
else
	skip "decompress-nsz: no .nsz and .nsp in ${games#$root/}/decompress-nsz, or no package"
fi

# 10d. update-data: a game in build/test-games/update-data and its update in
# update-data/content. When the game opens its data, Eden lays the update's
# data over the game's, but only for a game it was told about, as its game
# list tells it of every game. The driver told it only of the update, so the
# update's code read the first release's data. Baki Hanma Blood Arena 2.0.4
# then ran out of stack at start and stayed black; at frame 1200 it shows
# its title screen.
update="$(ls "$games/update-data/content"/*.nsp 2>/dev/null | head -1)"
if [ -n "$(game_in update-data)" ] && [ -n "$update" ]; then
	r=$(sandboxed_game update-data 1300 --timeout 3600 --content "$update" --screenshot "1200=$work/update-data-1200.tga")
	lit=$(python3 "$here/tests/lit-pixels.py" "$work/update-data-1200.tga" 2>/dev/null || echo 0)
	if [ "$r" = 0 ] && [ "$lit" -gt 100000 ]; then
		pass "update-data: the picture at frame 1200 has $lit pixels that are not black"
	else
		fail "update-data (exit $r): the picture at frame 1200 has $lit pixels that are not black (more than 100000 expected); see build/gate/update-data.err"
	fi
else
	skip "update-data: no game in ${games#$root/}/update-data or no update in update-data/content"
fi

# 10e. title-after-loading: a game played from start to its title screen, in
# build/test-games/title-after-loading (Streets of Rage 4, first release).
# Two fixes, two checks:
# - Reads from the game's data take time (upstream charges 40 MB/s). The core
#   slept on the host's clock, which does not move the game's, so its reads
#   took none: its three loading threads ran ahead of the thread using what
#   they load, its 512 MB heap filled, and it stayed on its first screen.
#   Between frames 500 and 3000 it now plays its intro: the picture changes.
# - A viewport that negates Y (a swizzle that NVIDIA's GL applies itself and
#   that the core never asks for) was dropped, and the game's title screen
#   came out upside down. At frame 5000 the dark bar holding "PRESS ANY
#   BUTTON" is near the bottom (rows 640-659), darker than the art near the
#   top (rows 60-79), where an upside-down picture has it.
if [ -n "$(game_in title-after-loading)" ]; then
	r=$(sandboxed_game title-after-loading 5000 --timeout 7200 --screenshot "5000=$work/title-after-loading-5000.tga")
	changes=$(awk '$1 == "frame" && $2 >= 500 && $2 <= 3000 { for (i = 3; i <= NF; i++) if ($i ~ /^video=/) seen[$i] = 1 }
		END { n = 0; for (v in seen) n++; print n }' "$work/title-after-loading.out")
	if [ "$r" = 0 ] && [ "$changes" -ge 10 ]; then
		pass "title-after-loading: between frames 500 and 3000 the game showed $changes different pictures"
	else
		fail "title-after-loading (exit $r): between frames 500 and 3000 the game showed $changes different pictures (10 or more expected); see build/gate/title-after-loading.err"
	fi
	shot="$work/title-after-loading-5000.tga"
	bar=$(python3 "$here/tests/band-brightness.py" "$shot" 640 660 2>/dev/null || echo -1)
	art=$(python3 "$here/tests/band-brightness.py" "$shot" 60 80 2>/dev/null || echo -1)
	lit=$(python3 "$here/tests/lit-pixels.py" "$shot" 2>/dev/null || echo 0)
	if [ "$r" = 0 ] && [ "$lit" -gt 500000 ] && awk -v b="$bar" -v a="$art" 'BEGIN { exit !(b >= 0 && b < a / 2) }'; then
		pass "title-after-loading: the title screen is the right way up (rows 640-659 at $bar, rows 60-79 at $art; $lit pixels not black)"
	else
		fail "title-after-loading (exit $r): the title screen at frame 5000 is not the right way up (rows 640-659 at $bar, rows 60-79 at $art, the first under half the second expected; $lit pixels not black, more than 500000 expected)"
	fi
else
	skip "title-after-loading: no game in ${games#$root/}/title-after-loading"
fi

echo "$fails failed"
exit "$fails"
