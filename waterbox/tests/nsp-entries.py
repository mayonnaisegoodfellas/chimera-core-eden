#!/usr/bin/env python3
"""The files an .nsp (or .nsz) holds, a line each: the SHA-1 of the bytes
and the name, in the container's order.

    nsp-entries.py <file>

Two containers of the same files compare equal line for line, whatever the
padding of their tables of names.
"""
import hashlib
import struct
import sys

f = open(sys.argv[1], "rb")
head = f.read(16)
if head[:4] != b"PFS0":
    sys.exit(f"{sys.argv[1]}: not an .nsp")
count, size = struct.unpack_from("<II", head, 4)
rows = f.read(count * 24)
names = f.read(size)
base = 16 + count * 24 + size
for i in range(count):
    at, length, name_at, _ = struct.unpack_from("<QQII", rows, i * 24)
    name = names[name_at:names.index(b"\0", name_at)].decode()
    f.seek(base + at)
    digest = hashlib.sha1()
    left = length
    while left:
        chunk = f.read(min(left, 1 << 22))
        if not chunk:
            sys.exit(f"{sys.argv[1]}: {name} is cut short")
        digest.update(chunk)
        left -= len(chunk)
    print(digest.hexdigest(), name)
