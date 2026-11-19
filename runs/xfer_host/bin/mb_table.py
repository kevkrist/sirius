#!/usr/bin/env python3
"""mb_table.py <log> [<log> ...]: MB4P rows -> markdown table (finish ms and ms/GiB by schedule), plus best config per case."""
import sys, collections
rows = []
for path in sys.argv[1:]:
    for line in open(path, errors="replace"):
        if not line.startswith("ROW,MB4P,"): continue
        d = dict(kv.split("=", 1) for kv in line.strip().split(",")[2:])
        d["log"] = path.split("/")[-1].replace(".log", "")
        rows.append(d)
by = collections.OrderedDict()
for d in rows:
    by.setdefault((d["log"], d["gpus"], d["sources"], d["mb"]), []).append(d)
print("| log | G | batches on | MiB | config | schedule | finish ms (min) | ms/GiB | vs off | pinned MiB | scratch MiB | host waits | ok |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
sched = {"-1": "tip loop/PEER", "0": "fan-out", "1": "GPU OR", "2": "host OR"}
for (log, g, s, mb), L in by.items():
    off = next((float(d["finish_us"]) for d in L if d["mode"] == "off"), None)
    for d in L:
        f = float(d["finish_us"]); cfg = d["mode"] if d["mode"] == "off" else f'{d["mode"]} w{d["window"]}' + (f' t{d["threads"]}' if d["schedule"] == "2" else "")
        ratio = f"{off / f:.2f}x" if off and d["mode"] != "off" else "-"
        print(f'| {log} | {g} | {s} | {mb} | {cfg} | {d["transport"]} {sched.get(d["schedule"], d["schedule"])} | {f/1000:.2f} ({float(d["finish_min"])/1000:.2f}) | {float(d["ms_per_gib"]):.0f} | {ratio} | {float(d["host_mib"]):.0f} | {float(d["scratch_mib"]):.0f} | {d["host_waits"]} | {d["verified"]} |')
