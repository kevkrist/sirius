#!/usr/bin/env python3
"""nvtx_names.py <sqlite> <start,end> <substr>: list distinct NVTX range names containing substr in window, count, sum ms, and the 'Task' ranges."""
import sqlite3, sys
from collections import defaultdict
c = sqlite3.connect(sys.argv[1]); w0, w1 = map(int, sys.argv[2].split(",")); sub = sys.argv[3]
S = dict(c.execute("select id,value from StringIds"))
agg = defaultdict(lambda: [0, 0.0])
for s, e, t, tid in c.execute("select start,end,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
    n = t if t is not None else S.get(tid, "")
    if sub in n:
        import re
        k = re.sub(r"Task \d+", "Task N", n)
        agg[k][0] += 1; agg[k][1] += (e - s) / 1e6
for k, (n, ms) in sorted(agg.items(), key=lambda x: -x[1][1])[:25]:
    print(f"{n:6d} {ms:10.1f}  {k[:150]}")
