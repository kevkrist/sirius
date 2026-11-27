#!/usr/bin/env python3
"""r7_tables.py : final full-suite table of section 6 (2026-11-27 close-out). CPU only.
Per query and arm: median of hot iterations (iter >= 2) minus the tip median of the SAME tag (same lock holds).
Columns: o1/g2 (option 1, joins with a filter plan), k1 (o1 + scan throttle 1), f1 (S6 shape), fk1 (f1 + scan throttle 1),
bf2 (f1 + two-phase staging). Source tag per cell is printed; '-' = not run."""
import csv, glob, statistics as st
B = "/opt/dlami/nvme/dfperf/runs/eager"
def hot(tag, arm, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{tag}_{arm}[0-9]/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) * 1000 for r in rows}
        out += [t[i] for i in sorted(t) if i >= 2]
    return out
def cell(arm_tags, q):
    for arm, tag in arm_tags:
        v, t = hot(tag, arm, q), hot(tag, "tip", q)
        if v and t: return st.median(v) - st.median(t), len(v), max(v) - min(v), tag, st.median(t), max(t) - min(t)
    return None
cols = [("o1 (sw1)", [("g2", "fs2"), ("g2", "r7")]),
        ("k1 (sw1)", [("k1", "c4"), ("k1", "b2"), ("k1", "r7")]),
        ("f1 11-19", [("f1", "fs2")]),
        ("f1 11-27", [("f1", "r7")]),
        ("fk1 11-27", [("fk1", "r7")]),
        ("bf2 11-19", [("bf2", "fs2")])]
print("| q | tip 11-19 | tip 11-27 (spread) | " + " | ".join(c for c, _ in cols) + " |")
print("|" + "---|" * (len(cols) + 3))
tot = {c: [0.0, 0] for c, _ in cols}; t19 = t27 = 0.0; n27 = 0
for q in range(1, 23):
    a, b = hot("fs2", "tip", q), hot("r7", "tip", q)
    t19 += st.median(a) if a else 0
    if b: t27 += st.median(b); n27 += 1
    row = []
    for c, at in cols:
        r = cell(at, q)
        if r is None: row.append("-"); continue
        tot[c][0] += r[0]; tot[c][1] += 1
        row.append(f"{r[0]:+.1f} ({r[1]}, {r[2]:.0f}, {r[3]})")
    print(f"| {q} | {st.median(a):.1f} | " + (f"{st.median(b):.1f} ({max(b)-min(b):.0f})" if b else "-") + " | " + " | ".join(row) + " |")
print(f"| **sum** | {t19:.0f} | {t27:.0f} ({n27} q) | " + " | ".join(f"**{tot[c][0]:+.0f}** ({tot[c][1]} q)" for c, _ in cols) + " |")
