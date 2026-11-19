#!/usr/bin/env python3
"""qsum.py <quent_df.json> [q]: per join (median over hot iterations): site tasks, last-task computing ms (Publishing Task),
tpub ms from query start, consumer scans: first start, gap publication -> first scan, tasks started before publication."""
import json, sys, statistics as st, collections
d = json.load(open(sys.argv[1])); qf = int(sys.argv[2]) if len(sys.argv) > 2 else None
by = collections.defaultdict(list)
for r in d:
    if qf and r["q"] != qf: continue
    for j in r["joins"]: by[(r["q"], j["join"])].append((r, j))
med = lambda x: st.median(x) if x else float("nan")
for (q, J), lst in sorted(by.items()):
    js = [j for _, j in lst if "tpub" in j]
    if not js: print(f"q{q} j{J}: no site data ({lst[0][1].get('err')})"); continue
    wall = med([r["wall_ms"] for r, _ in lst])
    line = f"q{q} j{J} n={len(js)} wall={wall:.0f} site={js[0]['site']} kinds={js[0]['kinds']} tasks={js[0]['n_tasks']} last_task_ms={med([j['comp_last'] for j in js]):.1f} med_task_ms={med([j['comp_med'] for j in js]):.1f} tpub={med([j['tpub'] for j in js]):.0f}"
    print(line)
    names = collections.defaultdict(list)
    for j in js:
        for c in j.get("consumers", []): names[c["scan"]].append(c)
    for n, cs in names.items():
        print(f"     consumer {n[:60]} primary={cs[0]['primary']} tasks={cs[0]['n_tasks']} first_start={med([c['first_c0'] for c in cs]):.0f} gap_pub_to_first={med([c['gap_pub_to_first'] for c in cs]):.1f} (min {min(c['gap_pub_to_first'] for c in cs):.1f}) started_before_pub max={max(c['tasks_started_before_pub'] for c in cs)} last_end={med([c['last_end'] for c in cs]):.0f}")
