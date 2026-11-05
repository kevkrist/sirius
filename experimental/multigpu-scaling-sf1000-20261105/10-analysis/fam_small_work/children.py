#!/usr/bin/env python3
"""Aggregate NVTX ranges nested (same thread) inside parent ranges matching a substring, within a window.
usage: children.py <sqlite> <start> <end> <parent-substr> [maxdepth=2]"""
import sqlite3, sys, re
from collections import defaultdict
db, s0, e0, sub = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
md = int(sys.argv[5]) if len(sys.argv) > 5 else 2
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
by = defaultdict(list)
for st, en, text, tid, g in c.execute("select start,end,text,textId,globalTid from NVTX_EVENTS where eventType=59 and end is not null and start>=? and start<=?", (s0, e0)):
    n = text if text is not None else S.get(tid, "")
    by[g].append((st, en, re.sub(r"\(id=\d+\)", "", re.sub(r"Task \d+", "Task *", n))))
agg = defaultdict(lambda: [0, 0.0]); pw = 0; pn = 0
for g, rs in by.items():
    rs.sort(key=lambda r: (r[0], -r[1]))
    parents = [r for r in rs if sub in r[2]]
    for ps, pe, pn_ in parents:
        pw += pe - ps; pn += 1
        stack = []
        for st, en, n in rs:
            if st < ps or en > pe or (st == ps and en == pe and n == pn_): continue
            while stack and stack[-1] <= st: stack.pop()
            d = len(stack) + 1; stack.append(en)
            if d <= md:
                a = agg[(d, n)]; a[0] += 1; a[1] += (en - st) / 1e6
print(f"parents '{sub}': n={pn} wall={pw/1e6:.1f} ms")
for (d, n), a in sorted(agg.items(), key=lambda kv: -kv[1][1])[:30]:
    print(f"  d{d} {a[0]:6d} {a[1]:8.1f} ms  {n[:100]}")
