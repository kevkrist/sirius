#!/usr/bin/env python3
"""nsys_pipe_spans.py <sqlite> <start> <end> : per 'Pipeline K: OP (id=X)' NVTX op range: count, first start / last end (ms from window start), thread-ms;
and per 'Pipeline K Task' range span. Plus H2D GB (pinned-src -> device, all GPUs) moved inside each pipeline's task span."""
import sqlite3, sys, re
from collections import defaultdict
db, W0, W1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
op = defaultdict(lambda: [0, 1e30, 0, 0.0]); tk = defaultdict(lambda: [0, 1e30, 0, 0.0])
for s, e, t, ti in c.execute("select start,end,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1)):
    n = t if t is not None else S.get(ti, '')
    m = re.match(r'Pipeline (\d+): (.*)', n); m2 = re.match(r'Pipeline (\d+) Task \d+', n)
    d = op[(int(m.group(1)), m.group(2))] if m else (tk[int(m2.group(1))] if m2 else None)
    if d is None: continue
    d[0] += 1; d[1] = min(d[1], s); d[2] = max(d[2], e); d[3] += (e - s) / 1e6
for k in sorted(tk):
    v = tk[k]; print(f"P{k:<3} tasks n={v[0]:5} span {(v[1]-W0)/1e6:7.0f}-{(v[2]-W0)/1e6:7.0f}  task-thread-ms {v[3]:8.0f}  ops: " +
                     '; '.join(f"{o[1]} n={x[0]} {(x[1]-W0)/1e6:.0f}-{(x[2]-W0)/1e6:.0f} {x[3]:.0f}ms" for o, x in sorted(op.items()) if o[0] == k))
