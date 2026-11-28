#!/usr/bin/env python3
"""hsites.py <tag> <q...> : per DF scan site and arm (medians of hot iterations, warm + first hot dropped, both halves):
build PARTITION end, filter ready, first scan start, scan end, build CONCAT end, query wall. ms from query start.
Filter ready: PARTITION-path site = build PARTITION end (Quent; publication is inside the last PARTITION task);
one-shot site = log time of the claimant's `whole-build delivery slot` line minus the window begin (logs of the same runs).
Quent part = runs/eager/bin/esites.py (site triples from the tip reference telemetry)."""
import sys, os, re, glob, json, subprocess, statistics as st
from datetime import datetime
R = "/opt/dlami/nvme/dfperf"; H = f"{R}/runs/hybridfull"; tag = sys.argv[1]; qs = sys.argv[2:]
ORDER = ["T", "S", "H", "X", "HX", "HXF", "E", "EH", "EX", "EHX", "EHXF"]
arms = [a for a in ORDER if os.path.isdir(f"{R}/telemetry/hybridfull_{tag}_{a}_a")]
js = f"{H}/out/sites_{tag}.json"
if not os.environ.get("REUSE"):
    cmd = ["python3", f"{R}/runs/eager/bin/esites.py", js] + [f"{a}={R}/telemetry/hybridfull_{tag}_{a}_a,{R}/telemetry/hybridfull_{tag}_{a}_b" for a in arms] + ["--"] + qs
    open(f"{H}/out/sites_{tag}_full.md", "w").write(subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, ALL="1", REF=f"{R}/telemetry/hybridfull_{tag}_S_a")).stdout)
E_ = json.load(open(js))
def ts(l): return datetime.strptime(l[1:24], "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1000
def oneshot_ready(arm, q):
    out = {}
    for half in "ab":
        for f in glob.glob(f"{H}/{tag}_{arm}_{half}/*_q{q}/log_dir/sirius_*.log"):
            wins = []; cur = None
            for l in open(f, errors="replace"):
                if "[window] begin" in l: cur = {"t0": ts(l), "j": {}}
                elif cur is None: continue
                elif "[window] end" in l: wins.append(cur); cur = None
                elif "whole-build delivery slot" in l:
                    m = re.search(r"join (\d+) whole-build delivery slot (\d+) observed in ([\d.]+) ms", l)
                    cur["j"].setdefault(int(m.group(1)), []).append((ts(l) - cur["t0"], float(m.group(3))))
            wins = [w for w in wins if w["j"]][2:]
            for w in wins:
                for j, ev in w["j"].items():
                    c = max(ev, key=lambda e: e[1]); out.setdefault(j, []).append((c[0], c[1]))
    return {j: (st.median(x[0] for x in v), st.median(x[1] for x in v)) for j, v in out.items()}
f = lambda x: "-" if x is None or x != x else f"{x:.0f}"
print("| q | site (join; build PARTITION / probe scan) | arm | n | wall | PARTITION end | filter ready | one-shot ms | first scan start | scan end | build CONCAT end |"); print("|" + "---|" * 11)
res = {}
for q in qs:
    ready = {a: oneshot_ready(a, q) for a in arms if "H" in a}
    for si, site in enumerate(E_[q]["_sites"]):
        if not site["df"]: continue
        hj = int(re.search(r"HASH_JOIN\((\d+)\)", site["hj"]).group(1)) if site.get("hj") else None
        ref = E_[q][arms[0]]["sites"].get(str(si))
        if not ref: continue
        rows = []
        for a in arms:
            m = E_[q][a]["sites"].get(str(si))
            if not m: continue
            r = ready.get(a, {}).get(hj)
            fr = r[0] if r and r[1] > 0.05 else m["pub"]
            rows.append((a, E_[q][a]["n"], E_[q][a]["wall"], m["pub"], fr, r[1] if r else None, m["s_first"], m["s_end"], m["c_end"]))
        spread = max(x[4] for x in rows) - min(x[4] for x in rows)
        if spread < 3 and not os.environ.get("ALL"): continue
        lab = f"j{hj}; {site['b']} / {site['s'].split(' ')[0]}"
        for x in rows:
            print(f"| {q} | {lab} | {x[0]} | {x[1]} | {x[2]:.1f} | {f(x[3])} | {f(x[4])} | {'-' if x[5] is None else f'{x[5]:.1f}'} | {f(x[6])} | {f(x[7])} | {f(x[8])} |")
            res.setdefault(f"q{q} j{hj}", {})[x[0]] = dict(part_end=x[3], ready=x[4], oneshot=x[5], scan_start=x[6], scan_end=x[7], concat_end=x[8])
json.dump(res, open(f"{H}/out/sites_{tag}_compact.json", "w"), indent=1)
