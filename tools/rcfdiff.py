#!/usr/bin/env python3
"""Compare two RCF archives entry by entry (by name, content SHA-1).

Usage:
  rcfdiff.py <original.rcf> <patched.rcf> [--list]
Prints added / removed / changed entry counts grouped by directory, and with --list every changed name.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcf  # noqa: E402


def digest_entries(path):
    entries, _ = rcf.open_rcf(path)
    out = {}
    with open(path, "rb") as f:
        for e in entries:
            f.seek(e.offset)
            out[e.name.lower()] = (e.size, hashlib.sha1(f.read(e.size)).hexdigest())
    return out


def group(name):
    parts = name.replace("\\", "/").strip("/").split("/")
    return "/".join(parts[:3]) if len(parts) > 3 else "/".join(parts[:-1])


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    a, b = digest_entries(argv[1]), digest_entries(argv[2])
    added = sorted(set(b) - set(a))
    removed = sorted(set(a) - set(b))
    changed = sorted(n for n in set(a) & set(b) if a[n][1] != b[n][1])
    print("# %s: %d -> %d entries, added %d, removed %d, changed %d"
          % (os.path.basename(argv[1]), len(a), len(b), len(added), len(removed), len(changed)))
    for label, names in (("added", added), ("removed", removed), ("changed", changed)):
        groups = {}
        for n in names:
            groups[group(n)] = groups.get(group(n), 0) + 1
        for g, c in sorted(groups.items(), key=lambda kv: -kv[1])[:25]:
            print("  %-8s %5d  %s" % (label, c, g))
        if "--list" in argv:
            for n in names:
                print("    %s %s %s" % (label, n, b.get(n, a.get(n))[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
