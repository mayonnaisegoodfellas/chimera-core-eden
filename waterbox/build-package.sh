#!/bin/sh
# Builds the Eden core package, eden.chimeraCore: core.wbx (fixed name),
# waterbox.config, default_keybinds.json, file_slots.json, the licences and
# build.json, loaded through Chimera's one built-in generic adapter. Written
# to build/package/ and installed into a Chimera checkout's build/Cores.
#
# CI builds the package that is published (.github/workflows/chimera.yml) with
# this script too. It carries no console keys, system software or games:
# whoever runs the core brings their own.
#
# Usage: ./build-package.sh [-m <miniBox dir>] [-r <chimera root>] [-o <out dir>] [-n]
#   -n  package what build/ already holds (no guest build, no relink)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-}"
chimera_root="${CHIMERA_ROOT:-}"
out="$root/build/package"
build=1
while getopts "m:r:o:n" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		r) chimera_root="$OPTARG" ;;
		o) out="$OPTARG" ;;
		n) build=0 ;;
		*) exit 2 ;;
	esac
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass -r <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"
[ -n "$mb" ] || mb="$chimera_root/extern/chimera-common-minibox"
mb="$(cd "$mb" && pwd)"
export MINIBOX_DIR="$mb"

if [ "$build" = 1 ]; then
	sh "$here/build-guest.sh"
	sh "$here/build-core.sh" -m "$mb"
fi
[ -f "$root/build/wbx/core.wbx" ] || { echo "no build/wbx/core.wbx" >&2; exit 1; }

staging="$root/build/package-staging"
rm -rf "$staging"
mkdir -p "$staging" "$out"
cp "$root/build/wbx/core.wbx" "$staging/core.wbx"
# The package carries no debug sections (most of the file); the symbol table
# stays, so a crash address still names its function (addr2line -f), and
# build/wbx/core.wbx keeps everything for a debugger.
strip --strip-debug "$staging/core.wbx"
cp "$here/waterbox.config" "$staging/waterbox.config"
cp "$here/default_keybinds.json" "$staging/default_keybinds.json"
cp "$here/file_slots.json" "$staging/file_slots.json"
# the texts a person reads, a table per language (Chimera's docs/translations.md)
cp -r "$here/languages" "$staging/languages"
# the terms travel with the binary (waterbox/package-licenses.json)
python3 "$mb/source/guest/package-licenses.py" "$root" "$staging"

# ---- version: the commit, and the commit's date in UTC (never the build's) ----
core_version="${CORE_VERSION:-}"
if [ -z "$core_version" ]; then
	if commit="$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null)"; then
		# (the submodule's tree always differs from its commit: the patches
		# are applied to it, and apply-patches.sh has checked it is exactly
		# what the series leaves. Dirty means this repository's own files.)
		git -C "$root" diff --quiet --ignore-submodules=dirty HEAD 2>/dev/null || commit="$commit-dirty"
		core_version="$commit+local"
	else
		core_version="unversioned+local"
	fi
fi
core_version_date="$(TZ=UTC git -C "$root" log -1 --date=format-local:%Y-%m-%dT%H:%M:%SZ --format=%cd HEAD 2>/dev/null || true)"
python3 - "$staging/waterbox.config" "$core_version" "$core_version_date" <<'PYVER'
import json, sys
path, version, date = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as f:
    cfg = json.load(f)
cfg["version"] = version
if date:
    cfg["versionDate"] = date
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PYVER

# ---- provenance: what built this exact package (inputs only) ----
python3 - "$staging/build.json" "$root" "$mb" "$core_version" <<'PYPROV'
import json, subprocess, sys
out, root, mb, version = sys.argv[1:5]
def run(*cmd, default="unknown"):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return default
def git(where, *args, default="unknown"):
    return run("git", "-C", where, *args, default=default)
os_id = run("sh", "-c", ". /etc/os-release && printf '%s %s' \"$ID\" \"$VERSION_ID\"")
json.dump({
    "version": version,
    "source": {"commit": git(root, "rev-parse", "HEAD"),
               "origin": git(root, "config", "--get", "remote.origin.url", default=""),
               "dirty": "-dirty" in version},
    "toolchain": {"compiler": "gcc " + run("gcc", "-dumpfullversion"),
                  "binutils": run("sh", "-c", "ld --version | head -1 | grep -o '[0-9][0-9.]*$'"),
                  "target": "x86_64-linux-musl",
                  "musl": run("cat", mb + "/extern/musl/VERSION")},
    "guestKit": {"name": "miniBox", "commit": git(mb, "rev-parse", "--short=12", "HEAD")},
    "upstream": {"name": "Eden", "pin": git(root + "/extern/eden", "rev-parse", "--short=12", "HEAD")},
    "builtOn": os_id,
}, open(out, "w"), indent=2, sort_keys=True)
PYPROV

zip_path="$out/eden.chimeraCore"
rm -f "$zip_path"
# deterministic packaging: sorted entries, fixed timestamp and permissions,
# pinned compression - the package's SHA-1 is the core's identity (a movie
# cites it)
python3 - "$staging" "$zip_path" <<'PYEOF'
import hashlib, os, sys, tempfile, zipfile

staging, zip_path = sys.argv[1], sys.argv[2]
FIXED_DATE = (1980, 1, 1, 0, 0, 0)

def write_package(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, dirs, files in os.walk(staging):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(root, name)
                info = zipfile.ZipInfo(os.path.relpath(full, staging), date_time=FIXED_DATE)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o644 << 16
                with open(full, "rb") as f:
                    z.writestr(info, f.read())

write_package(zip_path)
with tempfile.NamedTemporaryFile(suffix=".zip") as tmp:
    write_package(tmp.name)
    again = hashlib.sha1(open(tmp.name, "rb").read()).hexdigest()
first = hashlib.sha1(open(zip_path, "rb").read()).hexdigest()
if first != again:
    sys.exit(f"packaging is not deterministic: {first} then {again}")
print(f"package sha1 {first}")
PYEOF

cores_dir="$chimera_root/build/Cores"
if [ -d "$chimera_root/build" ]; then
	mkdir -p "$cores_dir"
	cp "$zip_path" "$cores_dir/eden.chimeraCore"
	for cache in "$chimera_root"/build/CoreCache/eden-*; do
		[ -d "$cache" ] && rm -rf "$cache" || true
	done
	echo "installed -> $cores_dir/eden.chimeraCore"
fi
echo "packaged -> $zip_path"
