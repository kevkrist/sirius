#!/usr/bin/env python3
"""Verifier timing table. Usage: times.py <tag> [arm ...]  (arms default tip f1 fk1)
Per process: iterations 0 (warm) and 1 (first hot) dropped; samples pooled over the 2 processes of an arm; medians."""
import sys, glob, csv, statistics, os, re
V = os.environ.get("BASE", "/opt/dlami/nvme/dfperf/runs/veager")
tag = sys.argv[1]; arms = sys.argv[2:] or ["tip", "f1", "fk1"]
def samples(arm, q):
    out = []; per = []
    for n in (1, 2):
        for f in glob.glob(f"{V}/{tag}_{arm}{n}/tpch_*_q{q}/csv/runtimes.csv"):
            rows = [r for r in csv.DictReader(open(f)) if str(r.get("query", r.get("query_id", ""))).lstrip("q") == str(q)]
            vals = sorted(((int(r["iteration"]), float(r["runtime_s"]) * 1000) for r in rows))
            hot = [v for i, v in vals if i >= 2]
            out += hot; per.append(statistics.median(hot) if hot else None)
    return out, per
qs = sorted({int(re.search(r"_q(\d+)$", os.path.dirname(os.path.dirname(p))).group(1)) for p in glob.glob(f"{V}/{tag}_tip*/tpch_*/csv/runtimes.csv")})
print("| q | tip med (n, spread; tip1 / tip2) | " + " | ".join(f"{a} delta ms (%; n, spread; per-process)" for a in arms if a != "tip") + " |")
print("|---|---|" + "---|" * (len(arms) - 1))
tot = {a: 0.0 for a in arms}; tott = {a: 0.0 for a in arms}
for q in qs:
    t, tp = samples("tip", q)
    if not t: continue
    tm = statistics.median(t)
    row = f"| {q} | {tm:.1f} ({len(t)}, {max(t)-min(t):.0f}; " + " / ".join(f"{x:.1f}" for x in tp if x) + ") |"
    for a in arms:
        if a == "tip": continue
        s, sp = samples(a, q)
        if not s: row += " - |"; continue
        m = statistics.median(s); tot[a] += m - tm; tott[a] += tm
        row += f" {m-tm:+.1f} ({100*(m-tm)/tm:+.1f}%; {len(s)}, {max(s)-min(s):.0f}; " + " / ".join(f"{x-tm:+.1f}" for x in sp if x) + ") |"
    print(row)
print("| sum | | " + " | ".join(f"{tot[a]:+.1f} over tip {tott[a]:.0f}" for a in arms if a != "tip") + " |")
