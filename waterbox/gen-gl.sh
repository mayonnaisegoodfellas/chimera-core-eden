#!/bin/sh
# Generates both halves of the GPU bridge (build/generated-gl) from miniBox's
# master list, for every name glad 2's header knows. Eden's renderer loads
# its entry points through glad's loader, which the driver points at the
# bridge's lookup, so the install step's assignments to glad's own pointers are taken
# out: they would tie the wrappers to one glad's globals, and Eden's glad
# (1) is not the one the host half uses (2).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/chimera-common-minibox}"
out="$root/build/generated-gl"
mkdir -p "$out"
python3 "$mb/source/gl/gen-gl-bridge.py" "$here/glad/include/glad/gl.h" "$mb/source/gl/gl-entry-points.txt" "$out" >/dev/null
sed -i '/^\tglad_gl[A-Za-z0-9_]* = w_gl[A-Za-z0-9_]*;$/d' "$out/gl-bridge-guest.cpp"
