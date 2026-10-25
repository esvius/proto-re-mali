#!/usr/bin/env python3
"""Pure3D (.p3d) chunk tree reader.

File header: u32 0xFF443350 ("P3D\\xff"), u32 12, u32 file_size.
Chunk:       u32 id, u32 data_size (header + own payload, children start here),
             u32 total_size (header + payload + children).

Usage:
  p3d.py tree  <file.p3d|file.p3d.rz> [max_depth]
  p3d.py stats <archive.rcf>             chunk-id histogram over every .p3d in an archive
  p3d.py find  <archive.rcf> <hex-bytes> chunks whose payload contains a byte pattern
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcf  # noqa: E402

P3D_MAGIC = 0xFF443350


class Chunk:
    __slots__ = ("id", "offset", "data_size", "total_size", "children")

    def __init__(self, cid, offset, data_size, total_size):
        self.id, self.offset = cid, offset
        self.data_size, self.total_size = data_size, total_size
        self.children = []

    def payload(self, buf):
        return buf[self.offset + 12:self.offset + self.data_size]


def parse_chunks(buf, start, end):
    out = []
    pos = start
    while pos + 12 <= end:
        cid, dsize, tsize = struct.unpack_from("<3I", buf, pos)
        if dsize < 12 or tsize < dsize or pos + tsize > end:
            raise ValueError("bad chunk at 0x%x: id=%08x data=%d total=%d" % (pos, cid, dsize, tsize))
        c = Chunk(cid, pos, dsize, tsize)
        c.children = parse_chunks(buf, pos + dsize, pos + tsize)
        out.append(c)
        pos += tsize
    return out


def parse(buf):
    if buf[:4] == rcf.RZ_MAGIC:
        buf = rcf.unrz(buf)
    magic, hsize, fsize = struct.unpack_from("<3I", buf, 0)
    if magic != P3D_MAGIC:
        raise ValueError("not a P3D file")
    if fsize != len(buf):
        raise ValueError("P3D size mismatch: header %d, actual %d" % (fsize, len(buf)))
    return buf, parse_chunks(buf, hsize, fsize)


def walk(chunks, depth=0):
    for c in chunks:
        yield c, depth
        yield from walk(c.children, depth + 1)


def iter_archive_p3d(path):
    entries, _ = rcf.open_rcf(path)
    with open(path, "rb") as f:
        for e in entries:
            name = e.name[:-3] if e.name.endswith(".rz") else e.name
            if not name.endswith(".p3d"):
                continue
            f.seek(e.offset)
            yield e.name, f.read(e.size)


def main(argv):
    if len(argv) < 3 or argv[1] not in ("tree", "stats", "find"):
        print(__doc__)
        return 2
    if argv[1] == "tree":
        max_depth = int(argv[3]) if len(argv) > 3 else 99
        with open(argv[2], "rb") as f:
            buf, chunks = parse(f.read())
        for c, d in walk(chunks):
            if d <= max_depth:
                head = c.payload(buf)[:24]
                print("%s%08x data=%-8d total=%-9d %s" % ("  " * d, c.id, c.data_size, c.total_size, head.hex()))
        return 0
    if argv[1] == "stats":
        counts, sizes, files, errors = {}, {}, 0, 0
        for name, blob in iter_archive_p3d(argv[2]):
            try:
                buf, chunks = parse(blob)
            except ValueError as ex:
                errors += 1
                print("ERR %s: %s" % (name, ex), file=sys.stderr)
                continue
            files += 1
            for c, _ in walk(chunks):
                counts[c.id] = counts.get(c.id, 0) + 1
                sizes[c.id] = sizes.get(c.id, 0) + c.data_size - 12
        print("# %d p3d files, %d parse errors" % (files, errors))
        for cid in sorted(counts, key=lambda k: -sizes[k]):
            print("%08x count=%-8d payload_bytes=%d" % (cid, counts[cid], sizes[cid]))
        return 0
    pattern = bytes.fromhex(argv[3])
    for name, blob in iter_archive_p3d(argv[2]):
        buf, chunks = parse(blob)
        for c, d in walk(chunks):
            if pattern in c.payload(buf):
                print("%s chunk %08x depth %d size %d" % (name, c.id, d, c.data_size))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
