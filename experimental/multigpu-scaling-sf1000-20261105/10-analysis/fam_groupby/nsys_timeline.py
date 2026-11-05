#!/usr/bin/env python3
"""nsys_timeline.py <sqlite> <start_ns> <end_ns> [bin_ms] : per time bin, host->device GB/s (all GPUs; includes
pinned-src PtoP-classified copies), device->host GB/s, true peer GB/s, and kernel busy fraction per device."""
import sqlite3, sys
from collections import defaultdict
db, W0, W1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); B = float(sys.argv[4]) if len(sys.argv) > 4 else 50.0
c = sqlite3.connect(db); nb = int((W1 - W0) / 1e6 / B) + 1
def add(arr, s, e, val):  # spread val uniformly over [s,e)
    s = max(s, W0); e = min(e, W1)
    if e <= s: return
    rate = val / (e - s); t = s
    while t < e:
        b = int((t - W0) / 1e6 / B); be = W0 + (b + 1) * B * 1e6; x = min(e, be)
        arr[b] += rate * (x - t); t = x
h2d = [0.0] * nb; d2h = [0.0] * nb; p2p = [0.0] * nb; kb = defaultdict(lambda: [0.0] * nb)
for s, e, k, sk, dk, b in c.execute("select start,end,copyKind,srcKind,dstKind,bytes from CUPTI_ACTIVITY_KIND_MEMCPY where end>=? and start<=?", (W0, W1)):
    if sk in (0, 1) and dk == 2: add(h2d, s, e, b)
    elif sk == 2 and dk in (0, 1): add(d2h, s, e, b)
    elif sk == 2 and dk == 2 and k == 10: add(p2p, s, e, b)
for s, e, d in c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_KERNEL where end>=? and start<=?", (W0, W1)):
    add(kb[d], s, e, e - s)
devs = sorted(kb)
print(f"{'t_ms':>6} {'H2D GB/s':>8} {'D2H':>6} {'P2P':>6}  kernel-busy% " + ' '.join(f"d{d}" for d in devs))
for i in range(nb):
    sc = 1e9 * B / 1e3
    print(f"{i*B:6.0f} {h2d[i]/sc:8.1f} {d2h[i]/sc:6.1f} {p2p[i]/sc:6.1f}  " + ' '.join(f"{min(999,100*kb[d][i]/(B*1e6)):3.0f}" for d in devs))
