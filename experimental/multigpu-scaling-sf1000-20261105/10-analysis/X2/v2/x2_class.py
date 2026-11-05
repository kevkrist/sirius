#!/usr/bin/env python3
"""x2_class.py <quent_g1> <quent_g4> [label]: exchange-family decomposition (same method as ../x2_xchg.py).
Per exchange (PARTITION pipeline k + consumer pipeline k+1), hot iters 2-4 mean: PARTITION wall share + consumer GPU-origin
prepare (clone) wall share at g4 vs (same at g1)/4. Classes: forced (g1 partition thread < 10% of g4), natural, fan-in
(consumer has 1 task). Also per-query exchange share and total GPU-origin prepare share (all consumers, incl. MERGE_AGGREGATE).
Prints class table, per-query table and a summary line."""
import csv, sys, bisect, re
from collections import defaultdict, Counter
ITS = ["2", "3", "4"]
def load(qd):
    pl = {r["pipeline"]: (r["type_name"], r["label"]) for r in csv.DictReader(open(f"{qd}/pipelines.csv"))}
    T = defaultdict(list)
    for r in csv.DictReader(open(f"{qd}/tasks.csv")): T[r["query"]].append(r)
    res = defaultdict(Counter); lab = {}; qtot = defaultdict(Counter)
    for ql, ts in T.items():
        m = re.match(r"tpch_q(\d+)_iter(\d+)", ql)
        if not m or m.group(2) not in ITS: continue
        q = int(m.group(1))
        iv = [(int(t["preparing"]), int(t["finalize"])) for t in ts if t["preparing"] and t["finalize"]]
        ev = sorted([(s, 1) for s, e in iv] + [(e, -1) for s, e in iv])
        X = []; F = []; n = 0; acc = 0.0; last = ev[0][0]
        for t, d in ev:
            if t > last: acc += (t - last) / n if n else 0; last = t
            X.append(t); F.append(acc); n += d
        def Fat(t):
            i = bisect.bisect_right(X, t) - 1
            if i < 0: return 0.0
            if i + 1 < len(X) and X[i + 1] > X[i]: return F[i] + (F[i + 1] - F[i]) * (t - X[i]) / (X[i + 1] - X[i])
            return F[i]
        qtot[q]["iters"] += 1; qtot[q]["busy"] += F[-1] / 1e6
        for t in ts:
            if not (t["preparing"] and t["first_compute"] and t["finalize"]): continue
            p, f, z = int(t["preparing"]), int(t["first_compute"]), int(t["finalize"])
            tn, lb = pl.get(t["pipeline"], ("?", "?")); pid = int(tn.split()[-1]) if tn.split()[-1].isdigit() else -1
            k = (q, pid); lab[k] = lb; c = res[k]
            c["n"] += 1; c["in"] += int(t["in_bytes"] or 0); c["thr"] += (z - p) / 1e6; c["share"] += (Fat(z) - Fat(p)) / 1e6
            if t["origin"] == "GPU":
                c["gprep_thr"] += (f - p) / 1e6; c["gprep_share"] += (Fat(f) - Fat(p)) / 1e6
                qtot[q]["gprep_thr"] += (f - p) / 1e6; qtot[q]["gprep_share"] += (Fat(f) - Fat(p)) / 1e6
        res[(q, "iters")]["k"] += 1
    return res, lab, qtot
g1, l1, t1 = load(sys.argv[1]); g4, l4, t4 = load(sys.argv[2]); name = sys.argv[3] if len(sys.argv) > 3 else sys.argv[2]
cls = defaultdict(Counter); perq = defaultdict(Counter)
for (q, pid), lb in sorted(l4.items()):
    if not lb.startswith("PARTITION"): continue
    k1 = g1[(q, "iters")]["k"] or 1; k4 = g4[(q, "iters")]["k"] or 1
    a1 = g1.get((q, pid), Counter()); a4 = g4[(q, pid)]
    c1 = g1.get((q, pid + 1), Counter()); c4 = g4.get((q, pid + 1), Counter())
    p4t = a4["thr"] / k4; p1t = a1["thr"] / k1
    cl = "fan-in" if c4["n"] / k4 <= 1.01 else ("forced" if p1t < 0.1 * p4t else "natural")
    C = cls[cl]
    C["x"] += 1; C["GB"] += a4["in"] / 1e9 / k4
    C["p4thr"] += p4t; C["p4sh"] += a4["share"] / k4; C["p1thr"] += p1t; C["p1sh"] += a1["share"] / k1
    C["c4thr"] += c4["gprep_thr"] / k4; C["c4sh"] += c4["gprep_share"] / k4; C["c1sh"] += c1["gprep_share"] / k1
    C["frag4"] += a4["n"] / k4
    g4s = (a4["share"] + c4["gprep_share"]) / k4; g1s = (a1["share"] + c1["gprep_share"]) / k1 / 4
    C["g4tot"] += g4s; C["g1tot4"] += g1s
    perq[q][cl] += g4s - g1s; perq[q]["g4"] += g4s; perq[q]["g1_4"] += g1s
print(f"## {name}")
print("| class | exch | GB in | PARTITION tasks | g4 part thr/share ms | g1 part thr/share ms | g4 clone thr/share ms | g4 total share | g1 share/4 | excess ms |")
print("|---|---|---|---|---|---|---|---|---|---|")
T = Counter()
for cl in ["forced", "natural", "fan-in"]:
    C = cls[cl]; T.update(C)
    print(f"| {cl} | {C['x']} | {C['GB']:.1f} | {C['frag4']:.0f} | {C['p4thr']:.0f} / {C['p4sh']:.0f} | {C['p1thr']:.0f} / {C['p1sh']:.0f} | {C['c4thr']:.0f} / {C['c4sh']:.0f} | {C['g4tot']:.0f} | {C['g1tot4']:.0f} | {C['g4tot']-C['g1tot4']:.0f} |")
print(f"| total | {T['x']} | {T['GB']:.1f} | {T['frag4']:.0f} | {T['p4thr']:.0f} / {T['p4sh']:.0f} | {T['p1thr']:.0f} / {T['p1sh']:.0f} | {T['c4thr']:.0f} / {T['c4sh']:.0f} | {T['g4tot']:.0f} | {T['g1tot4']:.0f} | {T['g4tot']-T['g1tot4']:.0f} |")
gp4 = sum(v["gprep_share"] / v["iters"] for v in t4.values()); gp4t = sum(v["gprep_thr"] / v["iters"] for v in t4.values())
b4 = sum(v["busy"] / v["iters"] for v in t4.values()); b1 = sum(v["busy"] / v["iters"] for v in t1.values())
print(f"\nSUMMARY {name}: exch_excess_ms={T['g4tot']-T['g1tot4']:.0f} g4_exch_share={T['g4tot']:.0f} all_GPUorigin_prep thr={gp4t:.0f} share={gp4:.0f} busy_wall g4={b4:.0f} g1={b1:.0f}")
print("\nper-query: q, g4 exchange share, g1/4, excess forced/natural/fan-in")
for q in sorted(perq):
    P = perq[q]; print(f"Q{q}: {P['g4']:.0f} {P['g1_4']:.0f} | {P['forced']:.0f} / {P['natural']:.0f} / {P['fan-in']:.0f}")
