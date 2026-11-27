#!/usr/bin/env python3
"""vtable.py <tag> : pooled medians per arm, deltas vs T, EH-E and X-E with per-half deltas and a bootstrap 95% CI
(resampling hot iterations within each process), result identity vs T_a. Also author's m1 numbers side by side."""
import csv, glob, hashlib, re, statistics as st, sys, os, random
V = "/opt/dlami/nvme/dfperf/runs/vhybrid"; A = "/opt/dlami/nvme/dfperf/runs/hybrid"
def load(base, tag):
    data, res = {}, {}
    for d in glob.glob(f"{base}/{tag}_*_[ab]/tpch_*_q*"):
        m = re.search(rf"{tag}_([A-Z]+)_([ab])/tpch_.*_q(\d+)$", d)
        arm, half, q = m.group(1), m.group(2), int(m.group(3))
        f = f"{d}/csv/runtimes.csv"
        if not os.path.exists(f): continue
        t = [float(r["runtime_s"]) * 1000 for r in csv.DictReader(open(f)) if int(r["iteration"]) >= 2 and r["query"] != "TOTAL"]
        data.setdefault(q, {}).setdefault(arm, {})[half] = t
        rf = glob.glob(f"{d}/sirius/q{q}/result.txt")
        if rf: res.setdefault(q, {})[(arm, half)] = hashlib.md5(open(rf[0], "rb").read()).hexdigest()[:8]
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
tag = sys.argv[1]
D, R = load(V, tag); AD, _ = load(A, "m1")
arms = [a for a in ["T", "H", "E", "EH", "X", "EHV"] if any(a in D[q] for q in D)]
f = lambda x: "-" if x is None else f"{x:+.1f}"
print("| q | n/arm | T ms | T a-b | " + " | ".join(f"{a}-T" for a in arms if a != "T") + " | results |"); print("|---|" * (len(arms) + 4))
S = {a: 0.0 for a in arms}; Sn = {a: 0 for a in arms}
for q in sorted(D):
    t = med(D, q, "T"); v = D[q]["T"]
    sp = abs(st.median(v["a"]) - st.median(v["b"])) if len(v) == 2 else float("nan")
    cells = []
    for a in arms:
        if a == "T": continue
        m_ = med(D, q, a); cells.append(f(None if m_ is None else m_ - t))
        if m_ is not None: S[a] += m_ - t; Sn[a] += 1
    h = R.get(q, {}); ref = h.get(("T", "a")); diff = sorted({f"{a}{x}" for (a, x), val in h.items() if val != ref})
    print(f"| {q} | {sum(len(x) for x in v.values())} | {t:.1f} | {sp:.1f} | " + " | ".join(cells) + f" | {'identical' if not diff else 'DIFF ' + ','.join(diff)} |")
print("| sum | | | | " + " | ".join(f"{S[a]:+.0f} ({Sn[a]}q)" for a in arms if a != "T") + " | |")
for (a, b) in [("EH", "E"), ("X", "E"), ("X", "EH"), ("H", "T"), ("E", "T"), ("EHV", "EH")]:
    print(f"\n### {a} - {b} (ms): mine pooled, half a, half b, bootstrap 95% CI; author m1 pooled; all 4 processes pooled\n")
    print("| q | mine | half a | half b | 95% CI | author | mine+author |"); print("|---|---|---|---|---|---|---|")
    tm = ta = tc = 0.0; lo_s = hi_s = 0.0; n = 0
    for q in sorted(D):
        if a not in D[q] or b not in D[q] or len(D[q][a]) < 2 or len(D[q][b]) < 2: continue
        d = med(D, q, a) - med(D, q, b)
        ha = st.median(D[q][a]["a"]) - st.median(D[q][b]["a"]); hb = st.median(D[q][a]["b"]) - st.median(D[q][b]["b"])
        lo, hi = boot(D, q, a, b)
        au = med(AD, q, a) - med(AD, q, b) if med(AD, q, a) is not None and med(AD, q, b) is not None else None
        comb = None
        if au is not None:
            pa = sum(D[q][a].values(), []) + sum(AD[q][a].values(), []); pb = sum(D[q][b].values(), []) + sum(AD[q][b].values(), [])
            comb = st.median(pa) - st.median(pb)
        tm += d; n += 1; lo_s += lo; hi_s += hi
        if au is not None: ta += au; tc += comb
        print(f"| {q} | {d:+.1f} | {ha:+.1f} | {hb:+.1f} | [{lo:+.1f}, {hi:+.1f}] | {f(au)} | {f(comb)} |")
    print(f"| sum ({n}q) | {tm:+.0f} | | | (sum of bounds [{lo_s:+.0f}, {hi_s:+.0f}]) | {ta:+.0f} | {tc:+.0f} |")
