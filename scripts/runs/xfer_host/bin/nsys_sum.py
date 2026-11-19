#!/usr/bin/env python3
"""nsys_sum.py <label>=<nsys_df json> ...: per hot iteration, DF finish-chain ranges: wall, host syncs, bytes copied, API calls."""
import json, sys
names = ("dynfilter::accum::finish", "dynfilter::accum::host_pipeline", "dynfilter::accum::host_or", "dynfilter::accum::late_wait",
         "dynfilter::stage::gather", "dynfilter::replicate_devices")
print("| capture | iter | query wall ms | range | n | wall ms | max ms | host syncs | sync ms | copies | GiB copied | copy kinds (n, GiB) | API calls |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for arg in sys.argv[1:]:
    label, path = arg.split("=", 1)
    for it in json.load(open(path)):
        for n in names:
            x = it["ranges"].get(n)
            if not x: continue
            mc = x["mc"]; cn = sum(v[0] for v in mc.values()); cb = sum(v[1] for v in mc.values()) / 2**30
            kinds = " ".join(f"{k}:{v[0]},{v[1]/2**30:.2f}" for k, v in sorted(mc.items()))
            print(f'| {label} | {it["iter"]} | {it["wall"]:.0f} | {n.replace("dynfilter::","")} | {x["n"]} | {x["wall"]:.1f} | {x["max_wall"]:.1f} | {x["n_sync"]} | {x["sync_ms"]:.1f} | {cn} | {cb:.2f} | {kinds} | {x["n_api"]} |')
