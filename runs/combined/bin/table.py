#!/usr/bin/env python3
"""table.py <tag> [<tag2> ...]: per query pooled median per arm (hot iterations >= 2 of both processes), deltas vs T,
result identity (md5 of result.txt vs arm T pass a), factorial decomposition. Markdown to stdout."""
import csv, glob, hashlib, re, statistics as st, sys, os
B = "/opt/dlami/nvme/dfperf/runs/combined"
ORDER = ["T", "O", "S", "X", "E", "SX", "SE", "XE", "SXE"]
data, res, rcs = {}, {}, {}
for tag in sys.argv[1:]:
    for d in glob.glob(f"{B}/{tag}_*_[ab]/tpch_*_q*"):
        m = re.search(rf"{tag}_([A-Z]+)_([ab])/tpch_.*_q(\d+)$", d)
        arm, half, q = m.group(1), m.group(2), int(m.group(3))
        f = f"{d}/csv/runtimes.csv"
        if not os.path.exists(f): continue
        t = [float(r["runtime_s"]) * 1000 for r in csv.DictReader(open(f)) if int(r["iteration"]) >= 2]
        data.setdefault(q, {}).setdefault(arm, {}).setdefault(half, []).extend(t)
        rf = glob.glob(f"{d}/sirius/q{q}/result.txt")
        if rf: res.setdefault(q, {})[(arm, half)] = hashlib.md5(open(rf[0], "rb").read()).hexdigest()[:8]
qs = sorted(data)
arms = [a for a in ORDER if any(a in data[q] for q in qs)]
def med(q, a):
    v = data[q].get(a)
    return st.median(sum(v.values(), [])) if v else None
print("### Pooled medians, ms (n hot per arm in brackets when != 8)\n")
print("| q | " + " | ".join(arms) + " | results |"); print("|---|" + "---|" * (len(arms) + 1))
tot = {a: 0.0 for a in arms}; full = {a: True for a in arms}
for q in qs:
    cells = []
    for a in arms:
        m_ = med(q, a)
        if m_ is None: cells.append("-"); full[a] = False; continue
        n = sum(len(x) for x in data[q][a].values()); tot[a] += m_
        cells.append(f"{m_:.1f}" + ("" if n == 8 else f" [{n}]"))
    h = res.get(q, {}); ref = h.get(("T", "a"))
    diff = sorted({f"{a}{x}" for (a, x), v in h.items() if v != ref})
    cells.append("identical" if not diff else "DIFF: " + ",".join(diff))
    print(f"| {q} | " + " | ".join(cells) + " |")
print("| **sum** | " + " | ".join(f"**{tot[a]:.0f}**" if full[a] else f"({tot[a]:.0f})" for a in arms) + " | |")
print("\n### Delta vs T, ms (spread = |half a - half b| of the arm's process medians, T spread in column T)\n")
print("| q | T a-b | " + " | ".join(a for a in arms if a != "T") + " |"); print("|---|" + "---|" * len(arms))
dt = {a: 0.0 for a in arms}
for q in qs:
    t = med(q, "T"); v = data[q]["T"]
    sp = abs(st.median(v["a"]) - st.median(v["b"])) if len(v) == 2 else float("nan")
    cells = []
    for a in arms:
        if a == "T": continue
        m_ = med(q, a)
        if m_ is None: cells.append("-"); continue
        dt[a] += m_ - t; cells.append(f"{m_ - t:+.1f}")
    print(f"| {q} | {sp:.1f} | " + " | ".join(cells) + " |")
print("| **sum** | | " + " | ".join(f"**{dt[a]:+.0f}**" for a in arms if a != "T") + " |")
if all(a in arms for a in ["S", "X", "E", "SX", "SE", "XE", "SXE"]):
    print("\n### Overlap, ms: pair/all measured vs sum of the single gains (positive overlap = gain lost to overlap)\n")
    print("| q | S | X | E | S+X sum | SX meas | overlap | S+E sum | SE meas | overlap | X+E sum | XE meas | overlap | S+X+E sum | SXE meas | overlap |")
    print("|---|" + "---|" * 15)
    acc = [0.0] * 15
    for q in qs + ["sum"]:
        if q == "sum": row = acc
        else:
            t = med(q, "T"); g = {a: med(q, a) - t for a in arms if a != "T" and med(q, a) is not None}
            if len(g) < 7: continue
            row = [g["S"], g["X"], g["E"]]
            for pair in ("SX", "SE", "XE", "SXE"):
                s = sum(g[c] for c in pair); row += [s, g[pair], g[pair] - s]
            acc = [x + y for x, y in zip(acc, row)]
        print(f"| {q} | " + " | ".join(f"{x:+.1f}" for x in row) + " |")
