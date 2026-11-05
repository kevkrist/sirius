#!/usr/bin/env python3
"""Per-device stats in a report window: memcpy bytes by kind, union busy ms (kernels, per copy kind), kernel sum.
usage: devstats.py <sqlite> <start_ns> <end_ns>"""
import sqlite3, sys
from collections import defaultdict
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
c = sqlite3.connect(db)
mk = dict(c.execute("select id,label from ENUM_CUDA_MEMCPY_OPER"))


def union(iv):
    iv = sorted(iv); tot = 0; cs = ce = None
    for a, b in iv:
        if cs is None or a > ce:
            if cs is not None:
                tot += ce - cs
            cs, ce = a, b
        else:
            ce = max(ce, b)
    if cs is not None:
        tot += ce - cs
    return tot / 1e6


K = defaultdict(list); M = defaultdict(lambda: defaultdict(list))
MB = defaultdict(lambda: defaultdict(int)); MN = defaultdict(lambda: defaultdict(int))
for st, en, d in c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?", (s0, e0)):
    K[d].append((st, en))
for st, en, d, k, b in c.execute("select start,end,deviceId,copyKind,bytes from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (s0, e0)):
    kn = mk.get(k, k); M[d][kn].append((st, en)); MB[d][kn] += b; MN[d][kn] += 1
print(f"window {(e0 - s0) / 1e6:.1f} ms")
for d in sorted(set(K) | set(M)):
    ks = sum(b - a for a, b in K[d]) / 1e6
    line = f" dev{d}: kernels n={len(K[d])} sum {ks:.0f} ms union {union(K[d]):.0f} ms |"
    for kn in sorted(M[d]):
        u = union(M[d][kn])
        line += f" {kn}: n={MN[d][kn]} {MB[d][kn] / 1e9:.2f} GB union {u:.0f} ms ({MB[d][kn] / 1e6 / max(u, 1e-9):.1f} GB/s busy);"
    print(line)
