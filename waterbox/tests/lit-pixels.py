#!/usr/bin/env python3
"""How many pixels of a picture are not black?

    lit-pixels.py PICTURE

Reads an uncompressed 24- or 32-bit TGA file (as the test runners and
chimera-run write them, whatever the name ends in) with nothing but the
Python standard library and prints the number of pixels with a colour channel
above 16. Exit 0, or 2 when the file cannot be read.
"""
import struct
import sys

path = sys.argv[1]
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
pixels = data[start:start + width * height * size]
if len(pixels) < width * height * size:
    print(f"{path}: the file ends before the picture does", file=sys.stderr)
    sys.exit(2)
lit = 0
for i in range(0, len(pixels), size):
    if pixels[i] > 16 or pixels[i + 1] > 16 or pixels[i + 2] > 16:
        lit += 1
print(lit)
