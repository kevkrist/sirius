#!/usr/bin/env python3
"""mb4_table.py <log>: MB4 rows -> markdown, ms (ms/GiB) per mode."""
import sys, re, collections
rows = collections.OrderedDict()
for line in open(sys.argv[1]):
    if not line.startswith("ROW,MB4,"): continue
    d = dict(kv.split("=") for kv in line.strip().split(",")[2:])
    rows.setdefault((int(d["mask"]), int(d["mb"])), {})[d["xfer"]] = (float(d["finish_us"]), float(d["finish_min"]), float(d["finish_max"]))
names = {1: "root only (GPU 0)", 3: "pair 0-1", 5: "GPUs 0+2 (one per pair)", 15: "all 4", 4: "GPU 2 only (not the root)"}
modes = ["off", "star", "exchange", "pipe"]
print("| batches on | Bloom MiB | " + " | ".join(f"{m} ms (ms/GiB)" for m in modes) + " | pipe vs off | spread max-min, worst mode |")
print("|---|---|" + "---|" * (len(modes) + 2))
for (mask, mb), r in sorted(rows.items(), key=lambda kv: ([1, 3, 5, 15, 4].index(kv[0][0]), kv[0][1])):
    cells = [f"{r[m][0]/1000:.2f} ({r[m][0]/1000/(mb/1024):.0f})" if m in r else "-" for m in modes]
    spread = max((v[2] - v[1]) / v[0] * 100 for v in r.values())
    gain = f"{r['off'][0]/r['pipe'][0]:.2f}x" if "pipe" in r and "off" in r else "-"
    print(f"| {names.get(mask, mask)} | {mb} | " + " | ".join(cells) + f" | {gain} | {spread:.1f}% |")
