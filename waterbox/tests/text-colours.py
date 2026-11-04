#!/usr/bin/env python3
"""How much white and how much yellow text is in a part of a picture?

    text-colours.py PICTURE LEFT TOP RIGHT BOTTOM

Reads an uncompressed 24- or 32-bit TGA file (as the test runners write them)
with nothing but the Python standard library and prints two numbers: the
pixels of columns LEFT..RIGHT-1 and rows TOP..BOTTOM-1 (counted from the top)
that are white (every channel at least 180), and those that are yellow (red
and green at least 180, blue below 100). Exit 0, or 2 when the file cannot be
read.
"""
import struct
import sys

path = sys.argv[1]
left, top, right, bottom = (int(a) for a in sys.argv[2:6])
try:
    data = open(path, "rb").read()
except OSError as e:
    print(f"{path}: {e}", file=sys.stderr)
    sys.exit(2)
if len(data) < 18 or data[1] != 0 or data[2] != 2 or data[16] not in (24, 32):
    print(f"{path}: not an uncompressed 24- or 32-bit TGA", file=sys.stderr)
    sys.exit(2)
width, height = struct.unpack_from("<HH", data, 12)
size = data[16] // 8
start = 18 + data[0]
# bit 5 of the descriptor: rows stored from the top
from_top = bool(data[17] & 0x20)
if len(data) < start + width * height * size:
    print(f"{path}: the file ends before the picture does", file=sys.stderr)
    sys.exit(2)
white = yellow = 0
for y in range(max(top, 0), min(bottom, height)):
    row = y if from_top else height - 1 - y
    at = start + (row * width) * size
    for x in range(max(left, 0), min(right, width)):
        b, g, r = data[at + x * size:at + x * size + 3]
        if r >= 180 and g >= 180:
            if b >= 180:
                white += 1
            elif b < 100:
                yellow += 1
print(white, yellow)
