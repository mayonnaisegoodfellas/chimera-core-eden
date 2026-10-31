#!/usr/bin/env python3
"""Are two pictures the same, pixel for pixel?

    same-picture.py A B

Reads 8-bit PNG files (grey, RGB or RGBA, not interlaced, as Chimera's window
writes them) and uncompressed 24- or 32-bit TGA files (as chimera-run writes
them, whatever the name ends in) with nothing but the Python standard
library, and compares their colours; alpha is left out, since a window and a
file need not agree on it. Exit 0 when every pixel is the same and the first picture is not
all black, 1 when they differ or it is black, 2 when a file cannot be read.
"""
import struct
import sys
import zlib


def read_tga(path, data):
    if len(data) < 18 or data[1] != 0 or data[2] != 2 or data[16] not in (24, 32):
        print(f"{path}: neither a PNG nor an uncompressed 24- or 32-bit TGA")
        sys.exit(2)
    width, height = struct.unpack_from("<HH", data, 12)
    size = data[16] // 8
    start = 18 + data[0]
    pixels = data[start:start + width * height * size]
    if len(pixels) != width * height * size:
        print(f"{path}: {len(pixels)} bytes of pixels for {width}x{height}")
        sys.exit(2)
    rows = [pixels[y * width * size:(y + 1) * width * size] for y in range(height)]
    if not data[17] & 0x20:
        rows.reverse()  # stored from the bottom row up
    rgb = bytearray()
    for line in rows:
        for x in range(width):
            b, g, r = line[x * size:x * size + 3]
            rgb += bytes((r, g, b))
    return width, height, bytes(rgb)


def read_picture(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        print(f"{path}: {e.strerror}")
        sys.exit(2)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return read_tga(path, data)
    at, idat, head = 8, [], None
    while at + 8 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        at += 12 + length
    width, height, depth, colour, _, _, interlace = head
    channels = {0: 1, 2: 3, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace:
        print(f"{path}: a PNG of depth {depth}, colour type {colour}, interlace {interlace} is not read here")
        sys.exit(2)
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows, prev, at = [], bytearray(stride), 0
    for _ in range(height):
        kind = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    rgb = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            rgb += px * 3 if channels == 1 else px[:3]
    return width, height, bytes(rgb)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    wa, ha, a = read_picture(sys.argv[1])
    wb, hb, b = read_picture(sys.argv[2])
    if (wa, ha) != (wb, hb):
        print(f"the pictures are {wa}x{ha} and {wb}x{hb}")
        return 1
    lit = sum(1 for i in range(0, len(a), 3) if a[i] or a[i + 1] or a[i + 2])
    different = sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])
    print(f"{wa}x{ha}, {different} pixels differ, {lit} are not black")
    if lit == 0:
        print("the first picture is all black: nothing was compared")
        return 1
    return 0 if different == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
