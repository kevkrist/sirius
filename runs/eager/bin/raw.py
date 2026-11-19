#!/usr/bin/env python3
"""raw.py <tag> <arm,arm,...> <q>...: all iterations per process (ms), iteration 0 = warm, 1 = first hot (dropped in medians)."""
import csv, glob, sys
B = "/opt/dlami/nvme/dfperf/runs/eager"
tag = sys.argv[1]; arms = sys.argv[2].split(',')
for q in sys.argv[3:]:
    for a in arms:
        for d in sorted(glob.glob(f"{B}/{tag}_{a}[0-9]/tpch_*_q{q}")):
            rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
            t = [float(r["runtime_s"]) * 1000 for r in sorted(rows, key=lambda r: int(r["iteration"]))]
            print(f"q{q} {d.split('/')[-2]:10s} " + " ".join(f"{x:7.1f}" for x in t))
