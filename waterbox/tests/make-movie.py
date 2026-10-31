#!/usr/bin/env python3
"""A Chimera movie from a harness input script, for chimera-run.

    make-movie.py <script> <frames> <out.txt>

The script's lines are "F MASK A0 ... A9" (gate-harness.h). A movie row is
grouped as the engine groups the controls: the console (Touch X, Touch Y,
Touch), then each player (its four stick axes, then its sixteen buttons).
"""
import sys

AXES = 10
NEUTRAL = [0] * 8 + [32768, 32768]


def read_script(path):
    lines = []
    for raw in open(path):
        raw = raw.split("#")[0].split()
        if raw:
            axes = [int(v) for v in raw[2:2 + AXES]]
            lines.append((int(raw[0]), int(raw[1], 0), axes + NEUTRAL[len(axes):]))
    return lines


def at(lines, frame):
    mask, axes = 0, NEUTRAL
    for start, m, a in lines:
        if start <= frame:
            mask, axes = m, a
    return mask, axes


def row(mask, axes):
    def buttons(first, count):
        return "".join("P" if mask >> (first + b) & 1 else "." for b in range(count))
    def values(first, count):
        return "".join("%5d," % v for v in axes[first:first + count])
    return "|" + values(8, 2) + buttons(32, 1) + "|" + values(0, 4) + buttons(0, 16) + "|" + values(4, 4) + buttons(16, 16) + "|"


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    script = read_script(sys.argv[1])
    with open(sys.argv[3], "w", newline="\n") as out:
        for f in range(1, int(sys.argv[2]) + 1):
            out.write(row(*at(script, f)) + "\n")
