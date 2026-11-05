#!/usr/bin/env python3
"""Per device & copy kind in a window: count, GB, busy ms (union of intervals), GB/s while busy; plus kernel busy ms per device
and aggregate-HtoD timeline in N ms bins (GB/s).  usage: memcpy_dev.py <sqlite> <start> <end> [binms=10]"""
import sqlite3, sys
from collections import defaultdict
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); B = float(sys.argv[4]) if len(sys.argv) > 4 else 10
c = sqlite3.connect(db)
mk = dict(c.execute("select id,label from ENUM_CUDA_MEMCPY_OPER"))
def union(iv):
    iv.sort(); tot = 0; cs = ce = None
    for s, e in iv:
        if cs is None or s > ce:
            if cs is not None: tot += ce - cs
            cs, ce = s, e
        else: ce = max(ce, e)
    if cs is not None: tot += ce - cs
    return tot
mem = defaultdict(list); nb = defaultdict(float); cnt = defaultdict(int)
nbin = int((e0 - s0) / 1e6 / B) + 1; hb = [0.0] * nbin
for st, en, dev, kind, b in c.execute("select start,end,deviceId,copyKind,bytes from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=?", (s0, e0)):
    k = (dev, mk.get(kind, kind)); mem[k].append((st, en)); nb[k] += b; cnt[k] += 1
    if mk.get(kind) == "CUDA_MEMCPY_OPER_HTOD" or kind == 1:
        hb[int((st - s0) / 1e6 / B)] += b
ker = defaultdict(list); kn = defaultdict(int)
for st, en, dev in c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and start<=?", (s0, e0)):
    ker[dev].append((st, en)); kn[dev] += 1
W = (e0 - s0) / 1e6
print(f"window {W:.1f} ms")
for k in sorted(mem):
    u = union(mem[k]) / 1e6
    print(f"  dev{k[0]} {k[1]:28s} n={cnt[k]:6d} {nb[k]/1e9:7.2f} GB busy {u:7.1f} ms  {nb[k]/1e9/max(u,1e-9)*1e3:6.1f} GB/s-busy  {nb[k]/1e9/W*1e3:6.1f} GB/s-window")
for d in sorted(ker):
    print(f"  dev{d} kernels n={kn[d]:6d} busy {union(ker[d])/1e6:7.1f} ms ({union(ker[d])/1e6/W:.2f} of window) sum {sum(e-s for s,e in ker[d])/1e6:.1f} ms")
print("  HtoD aggregate GB/s per %g ms bin:" % B, " ".join(f"{x/1e9/(B/1e3):.0f}" for x in hb))
