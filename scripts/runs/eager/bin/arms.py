#!/usr/bin/env python3
"""arms.py <tag> <arm,arm,...> [q ...]: median of hot iterations (iter >= 2) per arm, all processes <tag>_<arm><n>; delta vs first arm."""
import csv, glob, statistics as st, sys
B = "/opt/dlami/nvme/dfperf/runs/eager"
tag = sys.argv[1]; arms = sys.argv[2].split(','); qs = [int(x) for x in sys.argv[3:]] or list(range(1, 23))
def hot(arm, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{tag}_{arm}[0-9]/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) * 1000 for r in rows if r.get("query") in (f"q{q}", str(q))}
        out += [t[i] for i in sorted(t) if i >= 2]
    return out
print("| q | " + " | ".join(f"{a} med (n, min..max)" for a in arms) + " | " + " | ".join(f"{a}-{arms[0]}" for a in arms[1:]) + " |")
print("|" + "---|" * (2 * len(arms)))
tot = {a: 0.0 for a in arms}; complete = True
for q in qs:
    v = {a: hot(a, q) for a in arms}
    if not all(v.values()): print(f"| {q} | incomplete |"); complete = False; continue
    m = {a: st.median(v[a]) for a in arms}
    for a in arms: tot[a] += m[a]
    print(f"| {q} | " + " | ".join(f"{m[a]:.1f} ({len(v[a])}, {min(v[a]):.0f}..{max(v[a]):.0f})" for a in arms) + " | " + " | ".join(f"{m[a]-m[arms[0]]:+.1f}" for a in arms[1:]) + " |")
print(f"| sum{'' if complete else ' (complete rows)'} | " + " | ".join(f"{tot[a]:.0f}" for a in arms) + " | " + " | ".join(f"{tot[a]-tot[arms[0]]:+.0f}" for a in arms[1:]) + " |")
