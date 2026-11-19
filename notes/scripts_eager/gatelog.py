#!/usr/bin/env python3
"""gatelog.py <label> [q ...]: summarise the '[eager] join N probe activated|held' INFO lines of runs/eager/<label>/tpch_*_q<q>/log_dir.
Per (q, join): lines, activated / held, filter_plan, relevant channels / pending (max), started scans / with channel / pending (max), min GPU GiB available (min)."""
import glob, re, sys, collections
B = "/opt/dlami/nvme/dfperf/runs/eager"
label = sys.argv[1]; qs = [int(x) for x in sys.argv[2:]] or list(range(1, 23))
rx = re.compile(r"\[eager\] join (\d+) probe (activated|held) (.*?) \((.*)\)")
print("| q | join | lines | activated | held | plan | relevant ch | relevant pending (max) | started scans | with channel | started pending (max) | min GPU avail GiB | when |")
print("|" + "---|" * 13)
tot = collections.Counter()
for q in qs:
    agg = {}
    for f in glob.glob(f"{B}/{label}/tpch_*_q{q}/log_dir/sirius_*.log"):
        for line in open(f, errors="replace"):
            m = rx.search(line)
            if not m: continue
            kv = dict(x.split("=") for x in m.group(4).split(", ") if "=" in x)
            a = agg.setdefault(int(m.group(1)), dict(n=0, act=0, held=0, plan=kv.get("filter_plan"), rc=0, rp=0, ss=0, sf=0, sp=0, mem=1e9, when=set()))
            a["n"] += 1; a["act" if m.group(2) == "activated" else "held"] += 1; a["when"].add("finalize" if "finalize" in m.group(3) else "publication")
            for k, s in (("rc", "relevant_channels"), ("rp", "relevant_pending"), ("ss", "started_scans"), ("sf", "started_filtered"), ("sp", "started_pending")):
                if s in kv: a[k] = max(a[k], int(kv[s]))
            if "min_gpu_avail_gib" in kv: a["mem"] = min(a["mem"], float(kv["min_gpu_avail_gib"]))
    for j, a in sorted(agg.items()):
        print(f"| {q} | {j} | {a['n']} | {a['act']} | {a['held']} | {a['plan']} | {a['rc']} | {a['rp']} | {a['ss']} | {a['sf']} | {a['sp']} | {a['mem']:.1f} | {'/'.join(sorted(a['when']))} |")
        tot["joins"] += 1; tot["rp"] += a["rp"] > 0; tot["sp"] += a["sp"] > 0; tot["held"] += a["held"]
print(f"\njoins {tot['joins']}, with relevant pending {tot['rp']}, with started-scan channel pending {tot['sp']}, held lines {tot['held']}")
