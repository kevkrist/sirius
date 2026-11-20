#!/usr/bin/env python3
"""nv_ranges.py <A.sqlite> <U.sqlite> [min_ms]: NVTX range totals per name over hot query windows (2..N), A vs U (sum ms, count)."""
import sqlite3, sys, collections
def load(path):
    c = sqlite3.connect(path).cursor()
    S = dict(c.execute('select id,value from StringIds'))
    nv = [(S.get(tid) if t is None else t, s, e, g) for t, tid, s, e, g in c.execute('select text,textId,start,end,globalTid from NVTX_EVENTS') if e is not None]
    qw = sorted((s, e) for n, s, e, g in nv if n == 'sirius::query')
    hot = qw[1:]
    tot = collections.defaultdict(lambda: [0.0, 0, 0.0])
    for n, s, e, g in nv:
        if not n: continue
        for ws, we in hot:
            if s >= ws and e <= we:
                t = tot[n]; t[0] += (e - s) / 1e6; t[1] += 1; t[2] = max(t[2], (e - s) / 1e6); break
    return tot, [(we - ws) / 1e6 for ws, we in hot]
a, wa = load(sys.argv[1]); u, wu = load(sys.argv[2]); mn = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
print("query walls A", [round(x) for x in wa], "U", [round(x) for x in wu])
rows = []
for n in set(a) | set(u):
    x = a.get(n, [0, 0, 0]); y = u.get(n, [0, 0, 0])
    if max(x[0], y[0]) >= mn: rows.append((y[0] - x[0], n, x, y))
for d, n, x, y in sorted(rows):
    print(f"{d:+9.1f} | {n[:70]:70s} | A {x[0]:9.1f} ms n={x[1]:5d} max {x[2]:7.1f} | U {y[0]:9.1f} ms n={y[1]:5d} max {y[2]:7.1f}")
