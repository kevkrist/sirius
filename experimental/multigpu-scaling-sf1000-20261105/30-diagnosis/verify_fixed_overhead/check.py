#!/usr/bin/env python3
"""Re-derive outside-Quent-window time per run; also Init->Executing (planning, inside window),
per-iter spread, inter-query gaps. usage: check.py run_dir:quent_dir ..."""
import csv, sys, statistics as st
def load(run, qd):
    t = {}
    for r in csv.DictReader(open(f"{run}/timings.csv")):
        if r["query"].isdigit(): t[(int(r["query"]), int(r["iter"]))] = float(r["seconds"]) * 1000
    q = {}
    for r in csv.DictReader(open(f"{qd}/queries.csv")):
        lab = r["query"]; qn = int(lab.split("_")[1][1:]); it = int(lab.split("iter")[1])
        q[(qn, it)] = (int(r["init"]), int(r["executing"]), int(r["finished"]), float(r["wall_ms"]))
    return t, q
for spec in sys.argv[1:]:
    run, qd = spec.split(":")
    t, q = load(run, qd)
    out = {}; plan = {}; sds = []; gaps = []
    keys = sorted(q, key=lambda k: q[k][0])
    for a, b in zip(keys, keys[1:]):
        if b[1] >= 2: gaps.append((b, (q[b][0] - q[a][2]) / 1e6))
    for k in t:
        if k[1] >= 2 and k in q:
            out.setdefault(k[0], []).append(t[k] - q[k][3])
            plan.setdefault(k[0], []).append((q[k][1] - q[k][0]) / 1e6)
    tot_out = sum(st.mean(v) for v in out.values()); tot_plan = sum(st.mean(v) for v in plan.values())
    maxsd = max(st.pstdev(v) for v in out.values())
    print(f"{run}: outside {tot_out:.0f} ms | init->executing {tot_plan:.0f} ms | max per-q iter sd outside {maxsd:.1f} ms | "
          f"inter-query gap (Finished->next Init, hot) sum/3 {sum(g for _,g in gaps)/3:.0f} ms | timings tot {sum(st.mean([t[(qq,i)] for i in (2,3,4)]) for qq in range(1,23)):.0f} ms")
    if "-v" in sys.argv[0:0]: pass
    print("   per-q outside:", " ".join(f"Q{k}:{st.mean(v):.1f}" for k, v in sorted(out.items())))
    print("   per-q plan   :", " ".join(f"Q{k}:{st.mean(v):.1f}" for k, v in sorted(plan.items())))
