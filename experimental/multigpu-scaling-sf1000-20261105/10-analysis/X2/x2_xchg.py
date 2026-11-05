#!/usr/bin/env python3
"""x2_xchg.py <quent_g1> <quent_g4> [iters]: per exchange (PARTITION pipeline k + consumer pipeline k+1), mean over hot iters:
PARTITION task thread-ms and wall share (integral of 1/n_active over preparing->finalize), consumer GPU-origin prepare
thread-ms and wall share, input GB, task counts; g1 vs g4. Writes CSV to stdout."""
import csv, sys, bisect, re
from collections import defaultdict, Counter
ITS = sys.argv[3].split(",") if len(sys.argv) > 3 else ["2", "3", "4"]
def load(qd):
    pl = {r["pipeline"]: (r["type_name"], r["label"]) for r in csv.DictReader(open(f"{qd}/pipelines.csv"))}
    T = defaultdict(list)
    for r in csv.DictReader(open(f"{qd}/tasks.csv")): T[r["query"]].append(r)
    res = defaultdict(Counter)  # (q, pid) -> stats
    lab = {}
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
        for t in ts:
            if not (t["preparing"] and t["first_compute"] and t["finalize"]): continue
            p, f, z = int(t["preparing"]), int(t["first_compute"]), int(t["finalize"])
            tn, lb = pl.get(t["pipeline"], ("?", "?")); pid = int(tn.split()[-1]) if tn.split()[-1].isdigit() else -1
            k = (q, pid); lab[k] = lb; c = res[k]
            c["n"] += 1; c["in"] += int(t["in_bytes"] or 0); c["thr"] += (z - p) / 1e6; c["share"] += (Fat(z) - Fat(p)) / 1e6
            c["prep_thr"] += (f - p) / 1e6; c["prep_share"] += (Fat(f) - Fat(p)) / 1e6
            if t["origin"] == "GPU": c["gprep_thr"] += (f - p) / 1e6; c["gprep_share"] += (Fat(f) - Fat(p)) / 1e6
        res[(q, "iters")]["k"] += 1
    return res, lab
g1, l1 = load(sys.argv[1]); g4, l4 = load(sys.argv[2])
w = csv.writer(sys.stdout)
w.writerow(["query", "pid", "label", "cons_label", "in_GB", "g1_in_GB", "g1_n", "g4_n", "g1_part_thr", "g4_part_thr", "g1_part_share", "g4_part_share",
            "g1_cons_n", "g4_cons_n", "g1_cons_gprep_thr", "g4_cons_gprep_thr", "g1_cons_gprep_share", "g4_cons_gprep_share", "g1_cons_thr", "g4_cons_thr", "g1_cons_share", "g4_cons_share"])
for (q, pid), lb in sorted(l4.items()):
    if not lb.startswith("PARTITION"): continue
    k1 = g1[(q, "iters")]["k"] or 1; k4 = g4[(q, "iters")]["k"] or 1
    a1 = g1.get((q, pid), Counter()); a4 = g4[(q, pid)]
    c1 = g1.get((q, pid + 1), Counter()); c4 = g4.get((q, pid + 1), Counter())
    f = lambda x, k: round(x / k, 2)
    w.writerow([q, pid, lb, l4.get((q, pid + 1), "?")[:60], f(a4["in"] / 1e9, k4), f(a1["in"] / 1e9, k1), f(a1["n"], k1), f(a4["n"], k4), f(a1["thr"], k1), f(a4["thr"], k4),
                f(a1["share"], k1), f(a4["share"], k4), f(c1["n"], k1), f(c4["n"], k4), f(c1["gprep_thr"], k1), f(c4["gprep_thr"], k4),
                f(c1["gprep_share"], k1), f(c4["gprep_share"], k4), f(c1["thr"], k1), f(c4["thr"], k4), f(c1["share"], k1), f(c4["share"], k4)])
