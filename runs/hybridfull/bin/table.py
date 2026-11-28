#!/usr/bin/env python3
"""table.py <tag[,tag2]> : pooled medians per arm (hot iterations, first hot dropped), deltas vs S, pair deltas with
per-half deltas and bootstrap 95% CI (resampling hot iterations within each process), result identity vs T_a (or S_a)."""
import csv, glob, hashlib, re, statistics as st, sys, os, random
V = "/opt/dlami/nvme/dfperf/runs/hybridfull"
ORDER = ["T", "S", "H", "X", "HX", "HXF", "E", "EH", "EX", "EHX", "EHXF"]
PAIRS = [("HX", "X"), ("HXF", "X"), ("HXF", "HX"), ("EHX", "EX"), ("EHXF", "EX"), ("EHXF", "EHX"),
         ("H", "S"), ("EH", "E"), ("X", "S"), ("EX", "S"), ("EX", "X"), ("EHX", "HX"), ("S", "T")]
def load(tags):
    data, res = {}, {}
    for tag in tags.split(","):
      for d in glob.glob(f"{V}/{tag}_*_[ab]/tpch_*_q*"):
        m = re.search(rf"{tag}_([A-Z]+)_([ab])/tpch_.*_q(\d+)$", d)
        arm, half, q = m.group(1), tag + m.group(2), int(m.group(3))
        f = f"{d}/csv/runtimes.csv"
        if not os.path.exists(f): continue
        t = [float(r["runtime_s"]) * 1000 for r in csv.DictReader(open(f)) if r["query"] != "TOTAL" and int(r["iteration"]) >= 2]
        if not t: continue
        data.setdefault(q, {}).setdefault(arm, {})[half] = t
        rf = glob.glob(f"{d}/sirius/q{q}/result.txt")
        if rf:
            raw = open(rf[0], "rb").read()
            res.setdefault(q, {})[(arm, half)] = (hashlib.md5(raw).hexdigest()[:8], hashlib.md5(b"\n".join(sorted(raw.split(b"\n")))).hexdigest()[:8])
    return data, res
def med(d, q, a):
    v = d.get(q, {}).get(a)
    return st.median(sum(v.values(), [])) if v else None
def boot(d, q, a, b, n=4000):
    random.seed(1); out = []
    va, vb = d[q][a], d[q][b]
    for _ in range(n):
        ra = sum(([random.choice(x) for _ in x] for x in va.values()), [])
        rb = sum(([random.choice(x) for _ in x] for x in vb.values()), [])
        out.append(st.median(ra) - st.median(rb))
    out.sort(); return out[int(.025 * n)], out[int(.975 * n)]
D, R = load(sys.argv[1])
arms = [a for a in ORDER if any(a in D[q] for q in D)]
f = lambda x: "-" if x is None else f"{x:+.1f}"
print("| q | n S | S ms | S proc spread | " + " | ".join(f"{a}-S" for a in arms if a != "S") + " | results |"); print("|---" * (len(arms) + 4) + "|")
S = {a: 0.0 for a in arms}; Sn = {a: 0 for a in arms}
for q in sorted(D):
    t = med(D, q, "S"); v = D[q]["S"]
    pm = [st.median(x) for x in v.values()]; sp = max(pm) - min(pm)
    cells = []
    for a in arms:
        if a == "S": continue
        m_ = med(D, q, a); cells.append(f(None if m_ is None else m_ - t))
        if m_ is not None: S[a] += m_ - t; Sn[a] += 1
    h = R.get(q, {}); ref = h.get(("T", sorted(k[1] for k in h if k[0] == "T")[0])) if any(k[0] == "T" for k in h) else h.get(sorted(h)[0])
    diff = sorted({f"{a}{x}" for (a, x), val in h.items() if val[0] != ref[0]}); sdiff = [k for k, val in h.items() if val[1] != ref[1]]
    rtxt = "identical" if not diff else (f"tie order only ({len(diff)} procs)" if not sdiff else "DIFF " + ",".join(diff))
    print(f"| {q} | {sum(len(x) for x in v.values())} | {t:.1f} | {sp:.1f} | " + " | ".join(cells) + f" | {rtxt} ({len(h)}) |")
print("| sum | | | | " + " | ".join(f"{S[a]:+.0f} ({Sn[a]}q)" for a in arms if a != "S") + " | |")
for (a, b) in PAIRS:
    if a not in arms or b not in arms: continue
    print(f"\n### {a} - {b} (ms): pooled, per half, bootstrap 95% CI\n")
    print("| q | n/arm | pooled | halves | 95% CI | outside noise (CI excludes 0 and |d| > max(10 ms, 3%)) |"); print("|---|---|---|---|---|---|")
    tm = 0.0; lo_s = hi_s = 0.0; n = 0; sig = 0.0
    for q in sorted(D):
        if a not in D[q] or b not in D[q] or set(D[q][a]) != set(D[q][b]): continue
        d = med(D, q, a) - med(D, q, b)
        hs = " / ".join(f"{st.median(D[q][a][h]) - st.median(D[q][b][h]):+.1f}" for h in sorted(D[q][a]))
        lo, hi = boot(D, q, a, b)
        out = (lo > 0 or hi < 0) and abs(d) > max(10, 0.03 * med(D, q, b))
        tm += d; n += 1; lo_s += lo; hi_s += hi; sig += d if out else 0
        print(f"| {q} | {sum(len(x) for x in D[q][a].values())} | {d:+.1f} | {hs} | [{lo:+.1f}, {hi:+.1f}] | {'YES' if out else ''} |")
    print(f"| sum ({n}q) | | {tm:+.0f} | | (sum of bounds [{lo_s:+.0f}, {hi_s:+.0f}]) | outside-noise queries only: {sig:+.0f} |")
