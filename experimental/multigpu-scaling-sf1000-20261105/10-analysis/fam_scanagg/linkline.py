#!/usr/bin/env python3
"""Host-link timeline for one query window.
usage: linkline.py <sqlite> <start_ns> <end_ns> [bin_ms=25]
Host->device bytes = MEMCPY rows with srcKind=Pinned and dstKind=Device (copyKind H2D *or* PtoP: CUPTI labels copies
from pinned memory owned by another device's context as PtoP). Device->pinned = staging legs / result D2H.
Real P2P = srcKind=dstKind=Device and src!=dst device. Bytes are spread uniformly over each copy's duration.
Prints per-bin aggregate inbound GB/s, mean kernel-busy fraction across devices, and totals.
"""
import sqlite3, sys
from collections import defaultdict
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
B = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0
bn = int(B * 1e6)
c = sqlite3.connect(db)
nb = (e0 - s0) // bn + 1
inb = [0.0] * nb; outb = [0.0] * nb; p2p = [0.0] * nb
kb = defaultdict(lambda: [0.0] * nb)


def spread(arr, st, en, val):
    st = max(st, s0); en = min(en, e0)
    if en <= st:
        return
    dur = en - st
    i = (st - s0) // bn
    while st < en:
        be = s0 + (i + 1) * bn
        seg = min(en, be) - st
        arr[i] += val * seg / dur
        st += seg; i += 1


tot = defaultdict(float)
for st, en, b, sk, dk, sd, dd, d in c.execute(
        "select start,end,bytes,srcKind,dstKind,srcDeviceId,dstDeviceId,deviceId from CUPTI_ACTIVITY_KIND_MEMCPY where end>=? and start<=?", (s0, e0)):
    if sk == 1 and dk == 2:
        spread(inb, st, en, b); tot['host->dev'] += b
    elif sk == 2 and dk == 1:
        spread(outb, st, en, b); tot['dev->host'] += b
    elif sk == 2 and dk == 2 and sd is not None and dd is not None and sd != dd:
        spread(p2p, st, en, b); tot['p2p'] += b
devs = set()
for st, en, d in c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_KERNEL where end>=? and start<=?", (s0, e0)):
    spread(kb[d], st, en, en - st); devs.add(d)
nd = max(len(devs), 1)
print(f"window {(e0 - s0) / 1e6:.1f} ms; totals GB: " + ", ".join(f"{k} {v / 1e9:.2f}" for k, v in tot.items()))
print("bin_ms  in_GBps  out_GBps  p2p_GBps  kern_busy(mean/dev, can exceed 1 w/ concurrency)")
for i in range(nb):
    print(f"{i * B:6.0f} {inb[i] / bn:8.1f} {outb[i] / bn:8.1f} {p2p[i] / bn:8.1f}   {sum(kb[d][i] for d in devs) / nd / bn:5.2f}")
