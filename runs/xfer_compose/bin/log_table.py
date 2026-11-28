#!/usr/bin/env python3
"""log_table.py: SPIKE finish lines of the debug-log pass -> per query / mode: finishes, bytes, finish ms, ms/GiB, CPU OR ms."""
import glob, re, collections, sys
X = "/opt/dlami/nvme/dfperf/runs/xfer_compose"
pat = re.compile(r"reduced and replicated (\d+) key\(s\), (\d+) bytes in all, through (.*?): (\d+) contributing source\(s\), (\d+) target\(s\), (\d+) scratch.*finish_ms=([\d.]+) host_or_ms=([\d.]+) host_wait_ms=([\d.]+) host_window_bytes=(\d+)")
print("| q | mode | route | finishes (all iterations) | MiB each | sources | finish ms median | ms/GiB | CPU OR ms | host wait ms | pinned MiB | other-leader scratch MiB |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
import statistics as st
for q in sys.argv[1:]:
    for mode in ("off", "pipe", "compose"):
        ds = sorted(glob.glob(f"{X}/log_{mode}/tpch_*_q{q}"))
        if not ds: continue
        groups = collections.OrderedDict()
        for f in glob.glob(ds[-1] + "/log_dir/sirius_*.log"):
            for line in open(f, errors="replace"):
                m = pat.search(line)
                if m:
                    key = (int(m.group(2)), m.group(3), int(m.group(4)))
                    groups.setdefault(key, []).append((float(m.group(7)), float(m.group(8)), float(m.group(9)), int(m.group(10)), int(m.group(6))))
        for (b, route, src), v in sorted(groups.items(), key=lambda kv: -kv[0][0]):
            if b < (16 << 20): continue
            ms = st.median(x[0] for x in v)
            print(f"| {q} | {mode} | {route} | {len(v)} | {b/2**20:.0f} | {src} | {ms:.2f} | {ms/(b/2**30):.0f} | {st.median(x[1] for x in v):.2f} | {st.median(x[2] for x in v):.2f} | {v[0][3]/2**20:.0f} | {v[0][4]/2**20:.0f} |")
