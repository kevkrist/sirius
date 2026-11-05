#!/usr/bin/env python3
"""List NVTX ranges in a window: name (Pipeline names collapsed), count, total ms, first start/last end (ms rel window).
usage: nvtx_top.py <sqlite> <start_ns> <end_ns> [substr]"""
import sqlite3, sys, re
from collections import defaultdict
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); sub = sys.argv[4] if len(sys.argv) > 4 else ""
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
agg = defaultdict(lambda: [0, 0, 1e18, 0])
for st, en, text, tid in c.execute("select start,end,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and start<=?", (s0, e0)):
    n = text if text is not None else S.get(tid, "")
    if sub and sub not in n: continue
    n = re.sub(r"Task \d+", "Task *", n); n = re.sub(r"\(id=\d+\)", "", n)
    a = agg[n]; a[0] += 1; a[1] += (en - st) / 1e6; a[2] = min(a[2], (st - s0) / 1e6); a[3] = max(a[3], (en - s0) / 1e6)
for n, a in sorted(agg.items(), key=lambda kv: -kv[1][1])[:int(sys.argv[5]) if len(sys.argv) > 5 else 60]:
    print(f"{a[0]:6d} {a[1]:9.1f} ms  [{a[2]:7.1f} .. {a[3]:7.1f}]  {n[:110]}")
