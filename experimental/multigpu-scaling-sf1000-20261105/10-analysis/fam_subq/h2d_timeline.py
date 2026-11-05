#!/usr/bin/env python3
"""Host->device byte timeline inside a query window. usage: h2d_timeline.py <map.csv> <query> [bin_ms]
Prints total pinned-host->device bytes, per-device bytes, per-switch, and per-bin aggregate GB/s."""
import csv, sqlite3, sys
from collections import defaultdict
mp, q = sys.argv[1], sys.argv[2]; B = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
row = [r for r in csv.DictReader(open(mp)) if r['query'] == q and r['iter'] == '2'][0]
w0, w1 = int(row['range_start_ns']), int(row['range_end_ns'])
c = sqlite3.connect(row['sqlite'])
rows = list(c.execute("select start,end,bytes,deviceId,copyKind,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (w0, w1)))
h2d = [r for r in rows if r[5] in (0, 1) and r[6] == 2]
d2h = [r for r in rows if r[5] == 2 and r[6] in (0, 1)]
d2d_peer = [r for r in rows if r[5] == 2 and r[6] == 2 and r[4] == 10]
tot = sum(r[2] for r in h2d)
print(f"Q{q} window {(w1-w0)/1e6:.1f} ms; H2D {tot/1e9:.2f} GB in {len(h2d)} copies; D2H {sum(r[2] for r in d2h)/1e9:.2f} GB; dev-dev peer {sum(r[2] for r in d2d_peer)/1e9:.2f} GB")
pd = defaultdict(int)
for r in h2d: pd[r[3]] += r[2]
print("per-device H2D GB:", {k: round(v/1e9, 2) for k, v in sorted(pd.items())})
bins = defaultdict(float); sw = defaultdict(lambda: defaultdict(float))
for s, e, b, d, *_ in h2d:
    dur = max(e - s, 1)
    t = s
    while t < e:
        k = int((t - w0) / (B * 1e6)); be = w0 + (k + 1) * B * 1e6
        seg = min(e, be) - t
        bins[k] += b * seg / dur; sw[k][d // 2] += b * seg / dur
        t = min(e, be)
first = min(bins); last = max(bins)
print(f"H2D active from {first*B:.0f} to {(last+1)*B:.0f} ms")
for k in range(first, last + 1):
    gbs = bins[k] / 1e9 / (B / 1e3)
    print(f"  t={k*B:6.0f}ms agg={gbs:6.1f} GB/s swA={sw[k][0]/1e9/(B/1e3):5.1f} swB={sw[k][1]/1e9/(B/1e3):5.1f}")
