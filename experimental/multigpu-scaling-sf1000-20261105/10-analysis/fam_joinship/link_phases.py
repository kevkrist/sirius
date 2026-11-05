#!/usr/bin/env python3
"""link_phases.py <sqlite> <start,end>: per process-wide pipeline NVTX range ('Pipeline N: A -> B' style, first..last task of
each pipeline from 'Pipeline K Task' ranges), host->device bytes (copyKind 1, or 10 with srcKind pinned) and device->host bytes
issued inside the pipeline's [first task start, last task end] window, per device; plus achieved GB/s. Also kernel busy union per GPU."""
import sqlite3, sys, re
from collections import defaultdict
c = sqlite3.connect(sys.argv[1]); w0, w1 = map(int, sys.argv[2].split(","))
S = dict(c.execute("select id,value from StringIds"))
span = {}; lab = {}
for s, e, t, tid in c.execute("select start,end,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
    n = t if t is not None else S.get(tid, "")
    m = re.match(r"Pipeline (\d+) Task \d+ \[(.*)\]", n)
    if m:
        p = int(m.group(1)); a = span.get(p, [s, e]); span[p] = [min(a[0], s), max(a[1], e)]; lab[p] = m.group(2).split(" -> ")[0]
mem = list(c.execute("select start,end,deviceId,copyKind,srcKind,dstKind,bytes from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (w0, w1)))
def h2d(k, s, d): return k == 1 or (k == 10 and s in (0, 1) and d == 2)
def d2h(k, s, d): return k == 2 or (k == 10 and s == 2 and d in (0, 1))
tot_h = sum(b for st, en, dv, k, s, d, b in mem if h2d(k, s, d)); tot_d = sum(b for st, en, dv, k, s, d, b in mem if d2h(k, s, d))
print(f"query window {(w1-w0)/1e6:.0f} ms  H2D {tot_h/1e9:.1f} GB  D2H {tot_d/1e9:.1f} GB")
print(f"{'pipe':>4} {'op':14} {'start':>7} {'end':>7} {'ms':>6} {'H2D_GB':>7} {'GB/s':>6} {'D2H_GB':>7}  per-dev H2D GB")
for p in sorted(span, key=lambda p: span[p][0]):
    s0, e0 = span[p]; dur = (e0 - s0) / 1e9
    hb = defaultdict(float); db = 0
    for st, en, dv, k, s, d, b in mem:
        if st >= s0 and en <= e0:
            if h2d(k, s, d): hb[dv] += b
            elif d2h(k, s, d): db += b
    H = sum(hb.values())
    if dur * 1e3 < 5 and H < 1e8: continue
    print(f"{p:4d} {lab[p][:14]:14} {(s0-w0)/1e6:7.0f} {(e0-w0)/1e6:7.0f} {dur*1e3:6.0f} {H/1e9:7.2f} {H/1e9/max(dur,1e-9):6.1f} {db/1e9:7.2f}  " + " ".join(f"{dv}:{hb[dv]/1e9:.1f}" for dv in sorted(hb)))
