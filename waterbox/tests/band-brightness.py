#!/usr/bin/env python3
"""How bright is a band of rows of a picture?

    band-brightness.py PICTURE FIRST END

Reads an uncompressed 24- or 32-bit TGA file (as the test runners and
chimera-run write them, whatever the name ends in) with nothing but the
Python standard library and prints the mean of the red, green and blue values
(0 to 255) of rows FIRST to END - 1, counted from the top of the picture as
it is shown. Exit 0, or 2 when the file cannot be read or the rows are not in
it.
"""
import struct
import sys

path, first, end = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
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
if not 0 <= first < end <= height:
    print(f"{path}: rows {first} to {end - 1} are not in a picture {height} rows high", file=sys.stderr)
    sys.exit(2)
# (a TGA's rows run from the bottom up unless bit 5 of its descriptor is set)
top_first = data[17] & 0x20
total = 0
for y in range(first, end):
    row = y if top_first else height - 1 - y
    line = pixels[row * width * size:(row + 1) * width * size]
    for i in range(0, len(line), size):
        total += line[i] + line[i + 1] + line[i + 2]
print(round(total / (3 * width * (end - first)), 1))
