#!/usr/bin/env python3
"""Radical "ATG CORE CEMENT LIBRARY" (.rcf) archive reader, version 2.1 (little-endian).

Layout (observed on Prototype PC build 19788432):
  0x00  char[32]  magic "ATG CORE CEMENT LIBRARY" + NUL padding
  0x20  u8[4]     version 02 01 00 01
  0x24  u32       directory offset
  0x28  u32       directory size (count * 12)
  0x2C  u32       name table offset
  0x30  u32       name table size
  0x34  u32       reserved
  0x38  u32       file count
  dir:  count * {u32 name_hash, u32 offset, u32 size}   (sorted by hash)
  names: u32 alignment, u32 reserved, then count * {
           u32 timestamp, u32 alignment, u32 reserved, u32 len, char name[len], u8 pad[3] }
name_hash is the engine's core::HashFileName (matches all 18,044 entries of the shipped archives); names are
attached to directory entries by hash, with file-offset order as a fallback for archives where it fails.

Names mostly carry a ".rz" suffix: {char[4] "RZ\\0\\0", u32 0, u32 unpacked_size, u32 0} followed
by one zlib stream. `extract --unrz` writes the unpacked file without the ".rz" suffix.

Usage:
  rcf.py list    <archive.rcf>
  rcf.py extract <archive.rcf> <out_dir> [--unrz]
"""
import os
import struct
import sys
import zlib

RZ_MAGIC = b"RZ\0\0"


def unrz(data):
    magic, _res0, usize, _res1 = struct.unpack_from("<4sIII", data, 0)
    if magic != RZ_MAGIC:
        raise ValueError("not an RZ blob")
    out = zlib.decompress(data[16:])
    if len(out) != usize:
        raise ValueError("RZ size mismatch: %d != %d" % (len(out), usize))
    return out

MAGIC = b"ATG CORE CEMENT LIBRARY"


class Entry:
    __slots__ = ("hash", "offset", "size", "name", "timestamp")

    def __init__(self, h, off, size):
        self.hash, self.offset, self.size = h, off, size
        self.name = None
        self.timestamp = 0


def read_header(f):
    head = f.read(0x3C)
    if not head.startswith(MAGIC):
        raise ValueError("not an RCF archive")
    version = head[0x20:0x24]
    if version != b"\x02\x01\x00\x01":
        raise ValueError("unsupported RCF version %s" % version.hex())
    dir_off, dir_size, names_off, names_size, _res, count = struct.unpack_from("<6I", head, 0x24)
    return dir_off, dir_size, names_off, names_size, count


def read_names(f, names_off, names_size, count):
    f.seek(names_off)
    blob = f.read(names_size)
    pos = 8
    names = []
    for _ in range(count):
        ts, _align, _res, length = struct.unpack_from("<4I", blob, pos)
        pos += 16
        name = blob[pos:pos + length].rstrip(b"\0").decode("latin-1")
        pos += length + 3
        names.append((name, ts))
    return names


def name_hash(name, seed=0):
    """core::HashFileName: strip one leading '\\', then h = h * 31 + c over signed chars, with every char
    below 'a' shifted up by 0x20."""
    if name.startswith("\\"):
        name = name[1:]
    h = seed
    for b in name.encode("latin-1"):
        c = b - 256 if b >= 128 else b
        if c < 0x61:
            c += 0x20
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def open_rcf(path):
    with open(path, "rb") as f:
        dir_off, dir_size, names_off, names_size, count = read_header(f)
        f.seek(dir_off)
        raw = f.read(dir_size)
        entries = [Entry(*struct.unpack_from("<3I", raw, i * 12)) for i in range(count)]
        names = read_names(f, names_off, names_size, count)
    by_hash = {e.hash: e for e in entries}
    hashed = sum(1 for n, _ in names if name_hash(n) in by_hash)
    if hashed == count:
        for n, ts in names:
            e = by_hash[name_hash(n)]
            e.name, e.timestamp = n, ts
        mapping = "hash"
    else:
        # Fallback: names are stored in file-offset order.
        for e, (n, ts) in zip(sorted(entries, key=lambda e: e.offset), names):
            e.name, e.timestamp = n, ts
        mapping = "offset-order"
    return entries, mapping


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "extract"):
        print(__doc__)
        return 2
    entries, mapping = open_rcf(argv[2])
    if argv[1] == "list":
        print("# %d files, name mapping: %s" % (len(entries), mapping))
        for e in sorted(entries, key=lambda e: e.offset):
            print("%08x %10d %08x %s" % (e.offset, e.size, e.hash, e.name))
        return 0
    out = os.path.abspath(argv[3])
    do_unrz = "--unrz" in argv[4:]
    with open(argv[2], "rb") as f:
        for e in entries:
            rel = e.name.replace("\\", "/").lstrip("/")
            if do_unrz and rel.endswith(".rz"):
                rel = rel[:-3]
            dst = os.path.abspath(os.path.join(out, rel))
            if not dst.startswith(out + os.sep):
                raise ValueError("unsafe path in archive: %r" % e.name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(e.offset)
            with open(dst, "wb") as o:
                if do_unrz and e.name.endswith(".rz"):
                    o.write(unrz(f.read(e.size)))
                    continue
                left = e.size
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    o.write(chunk)
                    left -= len(chunk)
    print("extracted %d files (mapping: %s)" % (len(entries), mapping))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except BrokenPipeError:
        # Output piped into `head` and closed early; not an error.
        sys.stderr.close()
        sys.exit(0)
