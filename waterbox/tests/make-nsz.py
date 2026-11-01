#!/usr/bin/env python3
"""A small .nsz and the .nsp it stands for, made from nothing of anyone's:

    make-nsz.py <out folder>

writes solid.nsz (its archive compressed as one zstd stream), block.nsz (in
independent blocks) and expected.nsp. The archive is an NCA-shaped file of
made-up bytes: a 0x4000-byte header, then three sections - encrypted with
AES-CTR, left plain, encrypted with another key - and an .nsz holds it as
.ncz files do: the header as it is, the section table, and the rest
decrypted and compressed. A ticket-shaped file beside it is copied as it is.
Uses the zstd and openssl programs, which every runner has.
"""
import hashlib
import os
import struct
import subprocess
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def made_up(seed, n):
    """bytes that compress, but not to nothing"""
    data = bytearray()
    h = hashlib.sha256(seed).digest()
    while len(data) < n:
        h = hashlib.sha256(h).digest()
        data += h[:8] * 6 + h
    return bytes(data[:n])


def ctr(key, counter, offset, data):
    """AES-128-CTR as the archive's: the counter's high half the section's,
    its low half the offset / 16 (offsets are 16-aligned here)"""
    iv = counter[:8] + struct.pack(">Q", offset >> 4)
    return subprocess.run(["openssl", "enc", "-aes-128-ctr", "-nosalt", "-K", key.hex(), "-iv", iv.hex()],
                          input=data, stdout=subprocess.PIPE, check=True).stdout


def zstd(data):
    return subprocess.run(["zstd", "-q", "-c", "-19"], input=data, stdout=subprocess.PIPE, check=True).stdout


HEADER = 0x4000
header = made_up(b"header", HEADER)
sections = [  # offset, size, type (3 encrypted, 1 plain), key, counter
    (HEADER, 0x30000, 3, made_up(b"key0", 16), made_up(b"ctr0", 8) + bytes(8)),
    (HEADER + 0x30000, 0x8000, 1, bytes(16), bytes(16)),
    (HEADER + 0x38000, 0x21000, 3, made_up(b"key1", 16), made_up(b"ctr1", 8) + bytes(8)),
]
plain = made_up(b"body", sum(s[1] for s in sections))
nca = bytearray(header)
pos = 0
for off, size, kind, key, counter in sections:
    piece = plain[pos:pos + size]
    nca += ctr(key, counter, off, piece) if kind == 3 else piece
    pos += size

table = b"NCZSECTN" + struct.pack("<Q", len(sections))
for off, size, kind, key, counter in sections:
    table += struct.pack("<QQQQ", off, size, kind, 0) + key + counter
solid = header + table + zstd(plain)
exponent = 16
blocks = [plain[i:i + (1 << exponent)] for i in range(0, len(plain), 1 << exponent)]
packed = []
for b in blocks:
    z = zstd(b)
    packed.append(z if len(z) < len(b) else b)   # one no smaller compressed is stored as it is
block = (header + table + b"NCZBLOCK" + struct.pack("<BBBBIQ", 2, 1, 0, exponent, len(blocks), len(plain))
         + b"".join(struct.pack("<I", len(p)) for p in packed) + b"".join(packed))
ticket = made_up(b"ticket", 0x2C0)


def pfs0(files):
    names = b""
    rows = b""
    at = 0
    for name, data in files:
        rows += struct.pack("<QQII", at, len(data), len(names), 0)
        names += name.encode() + b"\0"
        at += len(data)
    while (16 + len(rows) + len(names)) % 0x20:
        names += b"\0"
    return b"PFS0" + struct.pack("<II", len(files), len(names)) + bytes(4) + rows + names + b"".join(d for _, d in files)


open(os.path.join(out, "solid.nsz"), "wb").write(pfs0([("0123456789abcdef.ncz", solid), ("game.tik", ticket)]))
open(os.path.join(out, "block.nsz"), "wb").write(pfs0([("0123456789abcdef.ncz", block), ("game.tik", ticket)]))
open(os.path.join(out, "expected.nsp"), "wb").write(pfs0([("0123456789abcdef.nca", bytes(nca)), ("game.tik", ticket)]))
print(f"{len(nca)} bytes of archive, {len(blocks)} blocks")
