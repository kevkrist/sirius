#!/usr/bin/env python3
"""dmon_summary.py <run_dir>: per-GPU PCIe rx/tx, SM, power, clock over the query phase (first label -> last label + 5 s)."""
import sys, glob, re, collections, statistics as st

r = sys.argv[1]
labs = [l[12:20] for l in open(glob.glob(f"{r}/log/*.log")[0]) if "sirius_set_query_label('tpch_q" in l]
s, e = labs[0], labs[-1]
d = collections.defaultdict(list)
for l in open(f"{r}/dmon.txt"):
    if l.startswith("#"): continue
    f = l.split()
    if len(f) < 15 or not (s <= f[0] <= e): continue

    def n(x):
        try: return float(x)
        except ValueError: return 0.0
    d[int(f[1])].append(dict(pwr=n(f[2]), sm=n(f[5]), mem=n(f[6]), rx=n(f[11]) / 1000, tx=n(f[12]) / 1000, pclk=n(f[14])))
print(f"{r}: window {s}..{e}")
for g, v in sorted(d.items()):
    rx = sorted(x["rx"] for x in v)
    act = [x for x in v if x["sm"] > 5]
    print(f" gpu{g} n={len(v)} rx mean {st.mean(rx):.1f} GB/s p50 {rx[len(rx)//2]:.1f} p90 {rx[int(.9*len(rx))]:.1f} max {rx[-1]:.1f}"
          f" | tx mean {st.mean(x['tx'] for x in v):.1f} | sm {st.mean(x['sm'] for x in v):.0f}% mem {st.mean(x['mem'] for x in v):.0f}%"
          f" pwr {st.mean(x['pwr'] for x in v):.0f}W pclk {st.mean(x['pclk'] for x in v):.0f} (sm>5% n={len(act)} pclk {st.mean(x['pclk'] for x in act) if act else 0:.0f}) rx_sum {sum(rx):.0f} GB")
