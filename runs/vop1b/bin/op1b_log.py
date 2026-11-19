#!/usr/bin/env python3
"""op1b_log.py <labels,comma> q [q ...]: per join, medians over hot iterations (window >= 2) of the deferred publication's
duration and of the join point (waited / publication done before). From the INFO log of prototype runs."""
import glob,re,sys,statistics as st,collections
B="/opt/dlami/nvme/dfperf/runs/vop1b"
R_RAN=re.compile(r"\[op1b\] deferred publication ran ([\d.]+) ms")
R_GATE=re.compile(r"\[op1b\] join (\d+) probe activation: (waited for the deferred publication|publication was done) ([\d.]+) ms")
R_WIN=re.compile(r"\[window\] (begin|end)")
print("| q | join | n | deferred publication ms med (min..max) | join point: waited ms med (n) | join point: done before ms med (n) |")
print("|---|---|---|---|---|---|")
for q in sys.argv[2:]:
    per=collections.defaultdict(lambda: dict(ran=[],wait=[],done=[]))
    for l in sys.argv[1].split(","):
        for d in sorted(glob.glob(f"{B}/{l}/tpch_*_q{q}")):
            for f in glob.glob(f"{d}/log_dir/sirius_*.log"):
                win=0; ran=None; gate=None; rows=[]
                for line in open(f,errors="replace"):
                    if "[window] begin" in line: win+=1; ran=gate=None; continue
                    m=R_RAN.search(line)
                    if m: ran=float(m.group(1))
                    m=R_GATE.search(line)
                    if m: gate=(m.group(1),m.group(2).startswith("waited"),float(m.group(3)))
                    if ran is not None and gate is not None:
                        rows.append((win,int(gate[0]),ran,gate[1],gate[2])); ran=gate=None
                # the last 4 windows that carry lines = hot iterations 2..5 (warm and first hot dropped)
                keep=sorted({r[0] for r in rows})[-4:]
                for w,j,r,waited,ms in rows:
                    if w in keep:
                        p=per[j]; p['ran'].append(r); (p['wait'] if waited else p['done']).append(ms)
    for j,p in sorted(per.items()):
        f=lambda v: f"{st.median(v):.1f} ({len(v)})" if v else "-"
        print(f"| {q} | {j} | {len(p['ran'])} | {st.median(p['ran']):.1f} ({min(p['ran']):.1f}..{max(p['ran']):.1f}) | {f(p['wait'])} | {f(p['done'])} |")
