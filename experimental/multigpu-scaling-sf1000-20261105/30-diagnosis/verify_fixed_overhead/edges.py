#!/usr/bin/env python3
"""Inside-window fixed edges per query (hot iters): head = Init -> first task preparing; tail = last task finalize -> Finished.
usage: edges.py <quent_dir>"""
import csv, sys, statistics as st
from collections import defaultdict
d = sys.argv[1]
Q = {r["query"]: (int(r["init"]), int(r["finished"])) for r in csv.DictReader(open(f"{d}/queries.csv"))}
first = defaultdict(lambda: 1 << 62); last = defaultdict(int)
for r in csv.DictReader(open(f"{d}/tasks.csv")):
    if r["preparing"]: first[r["query"]] = min(first[r["query"]], int(r["preparing"]))
    if r["finalize"]: last[r["query"]] = max(last[r["query"]], int(r["finalize"]))
H = defaultdict(list); T = defaultdict(list)
for lab, (i, f) in Q.items():
    if int(lab.split("iter")[1]) < 2 or lab not in last: continue
    qn = int(lab.split("_q")[1].split("_")[0])
    H[qn].append((first[lab] - i) / 1e6); T[qn].append((f - last[lab]) / 1e6)
h = sum(st.mean(v) for v in H.values()); t = sum(st.mean(v) for v in T.values())
print(f"{d}: head {h:.0f} ms tail {t:.0f} ms (suite, hot mean)")
print("  tail per q:", " ".join(f"Q{k}:{st.mean(v):.1f}" for k, v in sorted(T.items())))
