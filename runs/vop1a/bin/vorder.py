#!/usr/bin/env python3
"""vorder.py <runs root> <telemetry prefix> <label> <arm> <q> [--labels]
Independent ordering check (verifier), no join->scan mapping assumed:
for every [df_async] continuation (all iterations, warm included) list every task of ANY operator whose `Created`
falls inside the in-flight window [handoff, visible). A scan task there is a consumer that could have missed the filter
(if that scan is a target of that join). Also prints, per continuation, the first task Created after `visible` on each
scan operator and its distance.
"""
import collections, glob, re, sys

sys.path.insert(0, "/opt/dlami/nvme/dfperf/runs/profile/bin")
import qload  # noqa: E402

root, telp, label, arm, q = sys.argv[1:6]
show_labels = "--labels" in sys.argv
conts = []
for d in sorted(glob.glob(f"{root}/{label}/{arm}/tpch_*_q{q}")):
    for f in glob.glob(d + "/log_dir/sirius_*.log"):
        for l in open(f, errors="replace"):
            if "[df_async] continuation" in l:
                kv = dict(re.findall(r"(\w+)=(-?[\d.]+)", l))
                conts.append({k: float(v) for k, v in kv.items()})
tot_in = 0
scan_in = 0
n_cont = 0
min_gap = None
per_join = collections.defaultdict(lambda: dict(n=0, scans_in=0, other_in=collections.Counter(), min_gap=None))
for sess in sorted(glob.glob(f"{telp}_{label}/{arm}/q{q}/*/")):
    s = qload.load(sess)
    ops = s["ops"]
    if show_labels:
        for i, o in ops.items():
            print("OP", o["label"], o["pipe"])
        show_labels = False
    created = []  # (t, op label)
    for tid, ev in s["tasks"].items():
        pid = ev[0][3]["pipeline_uuid"]["target"]
        lab = ops[pid]["label"] if pid in ops else "?"
        for seq, t, k, v in ev:
            if k == "Created":
                created.append((t, lab))
    created.sort()
    qwin = [(qe["Executing"], qe["Finished"]) for qe in s["queries"].values() if "Finished" in qe and "Executing" in qe]
    for c in conts:
        h, v = c["handoff_epoch_ns"], c["visible_epoch_ns"] or c["end_epoch_ns"]
        w = [x for x in qwin if x[0] <= h <= x[1]]
        if not w:
            continue
        n_cont += 1
        J = int(c["join"])
        pj = per_join[J]
        pj["n"] += 1
        for t, lab in created:
            if h <= t < v:
                tot_in += 1
                if "GPU_SCAN" in lab or "SCAN" in lab:
                    scan_in += 1
                    pj["scans_in"] += 1
                    pj.setdefault("scan_labels", collections.Counter())[lab] += 1
                else:
                    pj["other_in"][re.sub(r"\(.*", "", lab)] += 1
        # first scan task created after visible, inside this query window
        nxt = [t for t, lab in created if t >= v and t <= w[0][1] and "SCAN" in lab]
        if nxt:
            g = (nxt[0] - v) / 1e6
            pj["min_gap"] = g if pj["min_gap"] is None else min(pj["min_gap"], g)
print(f"label={label} arm={arm} q={q}: continuations matched {n_cont} of {len(conts)}; tasks created in flight {tot_in}; scan tasks created in flight {scan_in}")
for J, pj in sorted(per_join.items()):
    print(f"  join {J}: n={pj['n']} scan tasks in flight={pj['scans_in']} {dict(pj.get('scan_labels', {}))} other tasks in flight={dict(pj['other_in'])} min(first later scan Created - visible)={pj['min_gap'] if pj['min_gap'] is None else round(pj['min_gap'], 3)} ms")
