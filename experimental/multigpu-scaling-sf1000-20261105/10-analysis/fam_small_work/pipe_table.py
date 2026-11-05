#!/usr/bin/env python3
"""Per-pipeline Quent accounting for one query label in two flattened runs.
usage: pipe_table.py <quentA> <quentB> <query> [iters=2,3,4]
Prints per pipeline (avg over iters): ntask, HOST GB, start/end ms (rel. to query executing),
thread-ms prepare (HOST / GPU origin), thread-ms compute, max task ms."""
import csv, sys
from collections import defaultdict
A, B, Q = sys.argv[1:4]
iters = [int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else "2,3,4").split(",")]

def load(d):
    qs = {r["query"]: r for r in csv.DictReader(open(f"{d}/queries.csv"))}
    pl = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{d}/pipelines.csv"))}
    tasks = defaultdict(list)
    for r in csv.DictReader(open(f"{d}/tasks.csv")):
        tasks[r["query"]].append(r)
    return qs, pl, tasks

def agg(d):
    qs, pl, tasks = load(d)
    out = defaultdict(lambda: defaultdict(float))
    walls = []
    for it in iters:
        lab = f"tpch_q{Q}_iter{it}"
        q = qs[lab]; t0 = int(q["executing"]); walls.append(float(q["wall_ms"]))
        per = defaultdict(list)
        for r in tasks[lab]:
            per[r["pipeline_type"]].append(r)
        for pt, rs in per.items():
            o = out[pt]
            o["label"] = pl.get(rs[0]["pipeline"], "?")
            o["n"] += len(rs) / len(iters)
            st = min(int(r["preparing"]) for r in rs if r["preparing"])
            en = max(int(r["finalize"]) for r in rs if r["finalize"])
            o["start"] += (st - t0) / 1e6 / len(iters); o["end"] += (en - t0) / 1e6 / len(iters)
            gpus = set()
            for r in rs:
                if not (r["preparing"] and r["first_compute"] and r["finalize"]):
                    continue
                prep = (int(r["first_compute"]) - int(r["preparing"])) / 1e6
                comp = (int(r["finalize"]) - int(r["first_compute"])) / 1e6
                if r["origin"] == "HOST":
                    o["prepH"] += prep / len(iters); o["hostGB"] += int(r["in_bytes"] or 0) / 1e9 / len(iters)
                else:
                    o["prepG"] += prep / len(iters)
                o["comp"] += comp / len(iters)
                gpus.add(r["gpu"])
            o["ngpu"] = max(o["ngpu"], len(gpus))
    return out, sum(walls) / len(walls)

a, wa = agg(A); b, wb = agg(B)
print(f"Q{Q}: wall A={wa:.1f} ms  B={wb:.1f} ms")
hdr = "pipe label                                        | A: n  GB   st-en(ms)     prepH  prepG  comp | B: n gpus  st-en(ms)    prepH  prepG  comp"
print(hdr)
keys = sorted(set(a) | set(b), key=lambda k: int(k.split()[-1]))
tot = defaultdict(float)
for k in keys:
    x, y = a.get(k, {}), b.get(k, {})
    lab = (x.get("label") or y.get("label"))[:48]
    print(f"{k.split()[-1]:>3} {lab:48s}| {x.get('n',0):4.0f} {x.get('hostGB',0):5.2f} {x.get('start',0):5.0f}-{x.get('end',0):5.0f} {x.get('prepH',0):6.0f} {x.get('prepG',0):6.0f} {x.get('comp',0):6.0f}"
          f" | {y.get('n',0):4.0f} {y.get('ngpu',0):1.0f} {y.get('start',0):5.0f}-{y.get('end',0):5.0f} {y.get('prepH',0):6.0f} {y.get('prepG',0):6.0f} {y.get('comp',0):6.0f}")
    for s, z in (("A", x), ("B", y)):
        for f in ("prepH", "prepG", "comp", "hostGB"):
            tot[s + f] += z.get(f, 0)
print(f"TOTAL A: prepH {tot['AprepH']:.0f} prepG {tot['AprepG']:.0f} comp {tot['Acomp']:.0f} hostGB {tot['AhostGB']:.2f} | B: prepH {tot['BprepH']:.0f} prepG {tot['BprepG']:.0f} comp {tot['Bcomp']:.0f} hostGB {tot['BhostGB']:.2f}")
