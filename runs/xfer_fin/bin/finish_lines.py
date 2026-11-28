#!/usr/bin/env python3
"""finish_lines.py <arm> [q ...]: Bloom accumulated-finish DEBUG lines of the id_ pass, last (hot) iteration.
Transfer arms print elapsed ms; arms without the transfer print only the transport."""
import glob, re, sys
B = "/opt/dlami/nvme/dfperf/runs/xfer_fin"; arm = sys.argv[1]; qs = [int(x) for x in sys.argv[2:]] or range(1, 23)
new = re.compile(r"united (\d+) key\(s\), (\d+) bytes per GPU, on (\d+) GPU\(s\) in ([\d.]+) ms: (\d+) island\(s\), (\d+) through the host \(([\d.]+) ms in host ORs, ([\d.]+) ms")
old = re.compile(r"reduced and replicated (\d+) key\(s\) of (\d+) bytes\s+through (peer copies|HOST staging): (\d+) contributing source\(s\), (\d+) target")
print("| q | finishes (hot iteration) | MiB per GPU | ms | ms/GiB | detail |"); print("|---|---|---|---|---|---|")
for q in qs:
    rows = []
    for f in sorted(glob.glob(f"{B}/id_{arm}A/tpch_*_q{q}/log_dir/sirius_*.log"))[-1:]:
        lines = open(f, errors="replace").read().splitlines()
        starts = [i for i, l in enumerate(lines) if "[window] begin" in l or "window] start" in l]
        cut = starts[-1] if starts else 0
        n_all = sum(1 for l in lines if "accumulated_bloom_builder::finish]" in l and ("united" in l or "reduced" in l))
        for l in lines[cut:] if starts else lines[len(lines) // 2:]:
            m = new.search(l)
            if m:
                k, b, g, ms, isl, hin, orm, wm = m.groups(); mib = int(b) / 2**20
                if int(hin) or float(ms) > 0.5: rows.append((mib, float(ms), f"{k} key(s), {g} GPUs, {isl} islands, {hin} via host, OR {float(orm):.1f} ms, wait {float(wm):.1f} ms"))
                continue
            m = old.search(" ".join(l.split()))
            if m:
                k, b, tr, s, t = m.groups(); rows.append((int(k) * int(b) / 2**20, None, f"{k} key(s), {tr}, {s} source(s), {t} target(s)"))
        if not rows: print(f"| {q} | 0 (lines in file: {n_all}) | | | | |")
        for mib, ms, d in sorted(rows, key=lambda r: -r[0]):
            print(f"| {q} | {len(rows)} | {mib:.1f} | {'' if ms is None else f'{ms:.1f}'} | {'' if ms is None or mib < 1 else f'{ms / mib * 1024:.0f}'} | {d} |")
