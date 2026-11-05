#!/usr/bin/env python3
"""Per-pipeline Quent accounting for a query, g1 vs g4, mean over hot iters.
usage: pipe_acct.py <quentA> <quentB> <qnum> [iters=2,3,4]"""
import csv, sys
from collections import defaultdict
A, B, qn = sys.argv[1], sys.argv[2], sys.argv[3]
iters = [int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else "2,3,4").split(",")]

def load(d):
    pipes = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{d}/pipelines.csv"))}
    ptype = {r["pipeline"]: r["type_name"] for r in csv.DictReader(open(f"{d}/pipelines.csv"))}
    q = {r["query"]: r for r in csv.DictReader(open(f"{d}/queries.csv"))}
    res = {}
    for it in iters:
        lab = f"tpch_q{qn}_iter{it}"
        t0 = int(q[lab]["init"]); wall = float(q[lab]["wall_ms"])
        P = defaultdict(lambda: dict(n=0, thr=0, prepH=0, prepG=0, comp=0, inb=0, s=1e30, e=0, gpus=defaultdict(int)))
        for r in csv.DictReader(open(f"{d}/tasks.csv")):
            if r["query"] != lab or not r["preparing"] or not r["finalize"]:
                continue
            key = (ptype[r["pipeline"]], pipes[r["pipeline"]])
            p = P[key]
            pr, fc, fi = int(r["preparing"]), int(r["first_compute"] or r["finalize"]), int(r["finalize"])
            p["n"] += 1; p["thr"] += (fi - pr) / 1e6
            if r["origin"] == "HOST": p["prepH"] += (fc - pr) / 1e6
            else: p["prepG"] += (fc - pr) / 1e6
            p["comp"] += (fi - fc) / 1e6
            p["inb"] += int(r["in_bytes"] or 0)
            p["s"] = min(p["s"], (pr - t0) / 1e6); p["e"] = max(p["e"], (fi - t0) / 1e6)
            p["gpus"][r["gpu"]] += 1
        res[it] = (wall, P)
    # average
    out = {}
    walls = [res[i][0] for i in iters]
    keys = set().union(*[res[i][1].keys() for i in iters])
    for k in keys:
        acc = defaultdict(float)
        for i in iters:
            p = res[i][1].get(k)
            if not p: continue
            for f in ("n", "thr", "prepH", "prepG", "comp", "inb", "s", "e"):
                acc[f] += p[f] / len(iters)
        out[k] = acc
    return sum(walls) / len(walls), out

wa, pa = load(A); wb, pb = load(B)
print(f"Q{qn} wall(quent, mean iters {iters}) A={wa:.1f} B={wb:.1f} ms")
def sk(k):
    return int(k[0].split()[-1])
hdr = f"{'pipe':>4} {'label':52} | {'nA':>5} {'thrA':>8} {'prepH':>7} {'prepG':>6} {'compA':>7} {'spanA':>15} | {'nB':>5} {'thrB':>8} {'prepH':>7} {'prepG':>6} {'compB':>7} {'spanB':>15} | {'GBA':>6} {'GBB':>6}"
print(hdr)
T = defaultdict(float)
for k in sorted(set(pa) | set(pb), key=sk):
    a = pa.get(k, defaultdict(float)); b = pb.get(k, defaultdict(float))
    print(f"{sk(k):>4} {k[1][:52]:52} | {a['n']:5.0f} {a['thr']:8.1f} {a['prepH']:7.1f} {a['prepG']:6.1f} {a['comp']:7.1f} {a['s']:7.0f}-{a['e']:7.0f} | {b['n']:5.0f} {b['thr']:8.1f} {b['prepH']:7.1f} {b['prepG']:6.1f} {b['comp']:7.1f} {b['s']:7.0f}-{b['e']:7.0f} | {a['inb']/1e9:6.2f} {b['inb']/1e9:6.2f}")
    for f in ("thr", "prepH", "prepG", "comp"):
        T[f + "A"] += a[f]; T[f + "B"] += b[f]
print("TOTAL thread-ms A: thr %.0f prepH %.0f prepG %.0f comp %.0f | B: thr %.0f prepH %.0f prepG %.0f comp %.0f" % (
    T["thrA"], T["prepHA"], T["prepGA"], T["compA"], T["thrB"], T["prepHB"], T["prepGB"], T["compB"]))
