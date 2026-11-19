#!/usr/bin/env python3
"""qsplit.py <tag> <q...>: Quent split per side for runs/vop4 telemetry (labels <tag>_A*, <tag>_B*).
Hot iterations = query index >= 2 in each session (index 0 warm, index 1 dropped).
Per side: median Quent wall, median union of DF build-site task time, median (wall - union),
per join: Publishing Task (last build-site task) compute ms, consumer scan tasks started before publication."""
import sys, glob, os, statistics as st, collections
sys.path.insert(0, "/opt/dlami/nvme/dfperf/runs/profile/bin")
import quent_df
T = "/opt/dlami/nvme/dfperf/telemetry/vop4"
tag = sys.argv[1]; qs = [int(x) for x in sys.argv[2:]]
med = lambda x: st.median(x) if x else float("nan")
for q in qs:
    rows = {}
    for side in "AB":
        recs = []
        for sess in sorted(glob.glob(f"{T}/{tag}_{side}[0-9]*/q{q}/*/")):
            try: recs += [r for r in quent_df.analyze(q, sess) if r["iter"] >= 2]
            except Exception as e: print("ERR", sess, e)
        if not recs: continue
        wall = [r["wall_ms"] for r in recs]; bu = [r["build_union_all"] for r in recs]
        outside = [r["wall_ms"] - r["build_union_all"] for r in recs]
        J = collections.defaultdict(lambda: collections.defaultdict(list))
        early = 0; ncons = 0
        for r in recs:
            for j in r["joins"]:
                if "comp_last" not in j: continue
                J[j["join"]]["pub"].append(j["comp_last"]); J[j["join"]]["union"].append(j["build_union"])
                J[j["join"]]["kinds"] = j["kinds"]
                for c in j.get("consumers", []):
                    ncons += c["n_tasks"]; early += c["tasks_started_before_pub"]
                    J[j["join"]]["gap"].append(c["gap_pub_to_first"]); J[j["join"]]["scan_comp"].append(c["comp_sum"])
        rows[side] = dict(n=len(recs), wall=med(wall), bu=med(bu), outside=med(outside), J=J, early=early, ncons=ncons,
                          wall_rng=(min(wall), max(wall)), out_rng=(min(outside), max(outside)))
    if len(rows) < 2: print(f"q{q}: incomplete"); continue
    a, b = rows["A"], rows["B"]
    print(f"q{q}: n A/B {a['n']}/{b['n']} | Quent wall {a['wall']:.1f} -> {b['wall']:.1f} ({b['wall']-a['wall']:+.1f}) | build-site union {a['bu']:.1f} -> {b['bu']:.1f} ({b['bu']-a['bu']:+.1f})"
          f" | outside {a['outside']:.1f} -> {b['outside']:.1f} ({b['outside']-a['outside']:+.1f}; ranges A {a['out_rng'][0]:.0f}-{a['out_rng'][1]:.0f} B {b['out_rng'][0]:.0f}-{b['out_rng'][1]:.0f})"
          f" | consumer scan tasks started before publication A {a['early']}/{a['ncons']} B {b['early']}/{b['ncons']}")
    for jn in sorted(set(a["J"]) | set(b["J"])):
        ja, jb = a["J"].get(jn, {}), b["J"].get(jn, {})
        f = lambda d, k: med(d.get(k, []))
        print(f"   j{jn} ({ja.get('kinds', '?')}): Publishing Task {f(ja,'pub'):.1f} -> {f(jb,'pub'):.1f} ms | site union {f(ja,'union'):.1f} -> {f(jb,'union'):.1f}"
              f" | min gap pub->first scan {f(ja,'gap'):.1f} -> {f(jb,'gap'):.1f} | consumer scan task-ms {f(ja,'scan_comp'):.0f} -> {f(jb,'scan_comp'):.0f}")
