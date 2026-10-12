#!/usr/bin/env python3
"""Aggregate pcsample rows by function for ONE unstripped binary.

usage: symbolize_bin.py pc.txt <unstripped-binary> [addr2line] [readelf]

The sampled binary may be a stripped copy of <unstripped-binary> (same build,
same addresses): rows are matched by basename. Defaults to the aarch64 cross
binutils, for a GS maburgs built with a static aarch64 toolchain; pass the
host addr2line/readelf for a host binary. symbolize.py is the drone variant
(Buildroot paths, shared libraries).
"""
import collections
import os
import subprocess
import sys

pc_path, binary = sys.argv[1], sys.argv[2]
A2L = sys.argv[3] if len(sys.argv) > 3 else "aarch64-linux-gnu-addr2line"
READELF = sys.argv[4] if len(sys.argv) > 4 else "aarch64-linux-gnu-readelf"


def loads(path):
    """[(p_offset, p_vaddr, p_filesz)] of the PT_LOAD segments."""
    out = subprocess.run([READELF, "-lW", path], capture_output=True, text=True).stdout
    segs = []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 6 and f[0] == "LOAD":
            segs.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
    return segs


def vaddr(off, segs):
    for o, v, sz in segs:
        if o <= off < o + sz:
            return off - o + v
    return off


hdr, rows = "", []
for line in open(pc_path):
    if line.startswith("#"):
        hdr = line.strip()
        continue
    n, ip, dso, off = line.split()
    rows.append((int(n), int(ip, 16), dso, int(off, 16)))
total = sum(r[0] for r in rows)
mine = [r for r in rows if os.path.basename(r[2]) == os.path.basename(binary)]
segs = loads(binary)
addrs = [hex(vaddr(r[3], segs)) for r in mine]
names = []
if addrs:
    out = subprocess.run([A2L, "-f", "-C", "-e", binary] + addrs,
                         capture_output=True, text=True).stdout.splitlines()
    names = out[0::2]
by_fn = collections.Counter()
for (n, _ip, _dso, _off), fn in zip(mine, names):
    by_fn[fn] += n
other = collections.Counter()
for n, _ip, dso, _off in rows:
    if os.path.basename(dso) != os.path.basename(binary):
        other[dso] += n

print(hdr)
print(f"user samples: {total}  in {os.path.basename(binary)}: {sum(by_fn.values())}")
for fn, n in by_fn.most_common(40):
    print(f"{100.0 * n / total:6.2f}%  {n:7d}  {fn[:150]}")
for dso, n in other.most_common(5):
    print(f"{100.0 * n / total:6.2f}%  {n:7d}  [{dso}]")
