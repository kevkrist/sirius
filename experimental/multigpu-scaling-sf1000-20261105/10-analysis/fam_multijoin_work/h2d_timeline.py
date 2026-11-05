#!/usr/bin/env python3
"""h2d_timeline.py <map.csv> <query> [bin_ms=10]: aggregate pinned-host->device bandwidth over the query's iter2 window
(copyKind any, srcKind=Pinned, dstKind=Device; attributed to dst device), plus GPU kernel-busy fraction per bin (union over
devices averaged). Prints compact per-bin rows and a summary: time with H2D >= 80% of ceiling, link-idle time."""
import sqlite3, csv, sys, collections
mp, q = sys.argv[1], int(sys.argv[2]); B = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
row = next(r for r in csv.DictReader(open(mp)) if int(r['query']) == q and r['iter'] == '2')
s, e = int(row['range_start_ns']), int(row['range_end_ns']); c = sqlite3.connect(row['sqlite'])
nb = int((e - s) / 1e6 / B) + 1
h = collections.defaultdict(lambda: [0.0] * nb)
def spread(arr, st, en, val):
    st = max(st, s); en = min(en, e)
    if en <= st: return
    rate = val / (en - st)
    t = st
    while t < en:
        b = int((t - s) / 1e6 / B); bend = s + (b + 1) * B * 1e6
        seg = min(en, bend) - t; arr[min(b, nb - 1)] += rate * seg; t += seg
for st, en, dev, dst, by, sk, dk in c.execute("select start,end,deviceId,dstDeviceId,bytes,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where end>=? and start<=?", (s, e)):
    if sk == 1 and dk == 2:
        d = dst if dst is not None else dev
        spread(h[d], st, en, by)
k = collections.defaultdict(lambda: [0.0] * nb)
for st, en, dev in c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_KERNEL where end>=? and start<=?", (s, e)):
    spread(k[dev], st, en, (en - st))  # ns busy (overlapping kernels may exceed bin)
devs = sorted(h) or [0]
tot = [sum(h[d][i] for d in devs) / (B * 1e-3) / 1e9 for i in range(nb)]  # GB/s
kb = [sum(min(k[d][i], B * 1e6) for d in k) / (B * 1e6) / max(len(k), 1) for i in range(nb)]
ceil = 105.0 if len(devs) > 1 else 52.0
print(f"Q{q} window {(e-s)/1e6:.0f} ms, devices {devs}, H2D total {sum(sum(v) for v in h.values())/1e9:.1f} GB, ceiling {ceil} GB/s")
line = []
for i in range(nb):
    line.append(f"{i*B:5.0f}:{tot[i]:4.0f}/{kb[i]*100:3.0f}%")
for j in range(0, nb, 8): print('  '.join(line[j:j+8]))
hi = sum(B for x in tot if x >= 0.8 * ceil); mid = sum(B for x in tot if 0.25 * ceil <= x < 0.8 * ceil); lo = sum(B for x in tot if x < 0.25 * ceil)
print(f"time H2D>=80% ceil: {hi:.0f} ms; 25-80%: {mid:.0f} ms; <25% (link ~idle): {lo:.0f} ms;  mean kernel-busy (per dev) {sum(kb)/nb*100:.0f}%")
