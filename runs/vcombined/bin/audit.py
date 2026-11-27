#!/usr/bin/env python3
"""audit.py <base dir> <tag-prefix or ''> : independent recomputation from raw run dirs.
Dirs: <base>/<prefix><ARM>_<a|b>/tpch_*_q<N>/ . Prints medians, deltas, switch evidence, result identity."""
import csv, glob, hashlib, re, statistics as st, sys, os, collections
base, pre = sys.argv[1], sys.argv[2]
T = collections.defaultdict(lambda: collections.defaultdict(dict))  # q, arm, half -> hot list
ev = {}; res = {}; n_iter = {}
for d in sorted(glob.glob(f"{base}/{pre}*_[ab]/tpch_*_q*")):
    m = re.search(rf"/{re.escape(pre)}([A-Z]+)_([ab])/tpch_.*_q(\d+)$", d)
    if not m: continue
    arm, half, q = m.group(1), m.group(2), int(m.group(3))
    rows = [r for r in csv.DictReader(open(f"{d}/csv/runtimes.csv")) if r["query"] != "TOTAL"]
    if (q, arm, half) in ev: half += "2"  # second block of the same query (verifier's repeat)
    n_iter[(q, arm, half)] = len(rows)
    T[q][arm][half] = [float(r["runtime_s"]) * 1000 for r in rows if int(r["iteration"]) >= 2]
    eager = blooms = shadow = 0; fin = 0.0; errs = 0; fb = 0
    for lf in glob.glob(f"{d}/log_dir/sirius_*.log"):
        for line in open(lf, errors="replace"):
            if "[eager] join" in line: eager += 1
            if "constructed filters published (Bloom" in line:
                blooms += 1; mm = re.search(r"([\d.]+) ms after the final", line); fin += float(mm.group(1)) if mm else 0
            if "[error]" in line or "[critical]" in line: errs += 1
            if re.search(r"execution fallback|fallback to DuckDB|falling back", line): fb += 1
    it = max(1, len(rows))
    ev[(q, arm, half)] = (eager / it, blooms / it, fin / it, errs, fb)
    rf = glob.glob(f"{d}/sirius/q{q}/result.txt")
    if rf:
        b = open(rf[0], "rb").read()
        res[(q, arm, half)] = (hashlib.md5(b).hexdigest()[:8], hashlib.md5(b"\n".join(sorted(b.split(b"\n")))).hexdigest()[:8], b.count(b"\n"))
    else: res[(q, arm, half)] = None
ORDER = ["T", "O", "S", "X", "E", "SX", "SE", "XE", "SXE"]
qs = sorted(T); arms = [a for a in ORDER if any(a in T[q] for q in qs)]
def med(q, a): return st.median(sum(T[q][a].values(), [])) if a in T[q] else None
def pmed(q, a):  # mean of the two process medians (alternative estimator)
    return st.mean(st.median(v) for v in T[q][a].values()) if a in T[q] else None
print("processes:", len(ev), " iterations/process:", collections.Counter(n_iter.values()))
print("\n## pooled medians ms | then delta vs T (pooled) | [alt: mean of process medians]")
print("| q | " + " | ".join(arms) + " |"); tot = collections.Counter(); alt = collections.Counter()
for q in qs:
    print(f"| {q} | " + " | ".join("-" if med(q, a) is None else f"{med(q,a):.1f}" for a in arms) + " |")
print("\n| q | Ta-Tb | " + " | ".join(a for a in arms if a != "T") + " |")
for q in qs:
    t = med(q, "T"); v = T[q]["T"]; sp = st.median(v["a"]) - st.median(v["b"]) if len(v) >= 2 else float("nan")
    cells = []
    for a in arms:
        if a == "T": continue
        if med(q, a) is None: cells.append("-"); continue
        d1 = med(q, a) - t; d2 = pmed(q, a) - pmed(q, "T"); tot[a] += d1; alt[a] += d2
        # paired per-half deltas
        hd = [st.median(T[q][a][h]) - st.median(T[q]["T"][h]) for h in sorted(T[q][a]) if h in T[q]["T"]]
        cells.append(f"{d1:+.1f} [{d2:+.1f}; a/b " + "/".join(f"{x:+.0f}" for x in hd) + "]")
    print(f"| {q} | {sp:+.1f} | " + " | ".join(cells) + " |")
print("| sum | | " + " | ".join(f"{tot[a]:+.1f} [{alt[a]:+.1f}]" for a in arms if a != "T") + " |")
print("\n## switch evidence per arm (per iteration, averaged over the arm's processes of a query): eager lines / Bloom publishes / finish ms")
print("| q | " + " | ".join(arms) + " |")
bad = []
for q in qs:
    cells = []
    for a in arms:
        e = [ev[(q, a, h)] for h in ("a","b","a2","b2") if (q, a, h) in ev]
        if not e: cells.append("-"); continue
        cells.append(f"{st.mean(x[0] for x in e):.0f}/{st.mean(x[1] for x in e):.0f}/{st.mean(x[2] for x in e):.0f}")
        for h in ("a","b","a2","b2"):
            x = ev.get((q, a, h))
            if x and (x[3] or x[4]): bad.append((q, a, h, x[3], x[4]))
    print(f"| {q} | " + " | ".join(cells) + " |")
print("error/fallback lines:", bad or "none")
# per-process consistency of switches: eager>0 iff E in arm
viol = []
for (q, a, h), x in ev.items():
    if a != "T" and ("E" in a) != (x[0] > 0) and any(ev[(q, b, hh)][0] > 0 for (qq, b, hh) in ev if qq == q): viol.append(("eager", q, a, h, x[0]))
    if a == "T" and x[0] > 0: viol.append(("eager-in-T", q, a, h))
print("eager switch violations:", viol or "none")
print("\n## results")
for q in qs:
    ref = res.get((q, "T", "a")); ks = [k for k in res if k[0] == q]
    if any(res[k] is None for k in ks): print(q, "MISSING result:", [k[1:] for k in ks if res[k] is None]); continue
    raw = {res[k][0] for k in ks}; srt = {res[k][1] for k in ks}
    if len(raw) > 1: print(f"q{q}: {len(raw)} raw md5 over {len(ks)} processes, sorted-content md5 {len(srt)} distinct, rows {set(res[k][2] for k in ks)}")
    elif res[ks[0]][2] == 0: print(f"q{q}: EMPTY result")
print("queries with all raw md5 identical:", [q for q in qs if len({res[k][0] for k in res if k[0] == q and res[k]}) == 1])
