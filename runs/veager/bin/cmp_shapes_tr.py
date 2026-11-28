#!/usr/bin/env python3
"""Verifier: per-window (= per statement) comparison of TRACE logs of the shape run, tip vs arm.
Usage: cmp_shapes_tr.py <tip dir> <arm dir>   (dirs under runs/veager/shapes)
Per window: DF key set, rows in/out of every DYNAMIC_FILTER, rows produced by GPU_SCAN / FILTER / HASH_JOIN / DYNAMIC_FILTER."""
import sys, re, glob, collections, json
V = "/opt/dlami/nvme/dfperf/runs/veager/shapes"
key_re = re.compile(r"\[dynamic_filter_key\].*join=(\d+) condition=(\d+).*?rows=(\d+).*?kind=(\S+) size=(\d+) zone_map=(\d+) pushed=(\d+) reason=(\S+)")
op_re = re.compile(r"Pipeline (\d+): operator (\w+) \(id=(\d+)\) task=\d+ (executing on|produced) \d+ batches, num rows: (\d+)")
def windows(arm):
    out = []; cur = None
    for f in sorted(glob.glob(f"{V}/{arm}/log/*.log")):
        for l in open(f, errors="replace"):
            if "[window] begin" in l: cur = dict(keys=set(), o=collections.Counter(), i=collections.Counter(), eager=0, held=0, ok=False); continue
            if cur is None: continue
            if "[window] end" in l: cur["ok"] = "outcome=ok" in l; out.append(cur); cur = None; continue
            m = key_re.search(l)
            if m: cur["keys"].add(m.groups()); continue
            m = op_re.search(l)
            if m:
                k = f"{m[2]}({m[3]})"
                if m[4] == "produced": cur["o"][k] += int(m[5])
                else: cur["i"][k] += int(m[5])
                continue
            if "[eager] join" in l:
                cur["eager"] += 1; cur["held"] += " probe held " in l
    return out
a, b = sys.argv[1], sys.argv[2]
A, B = windows(a), windows(b)
names = []
for l in open(f"{V}/{a}/results.jsonl"):
    j = json.loads(l)
    if "iter" in j: names.append(f"{j['shape']}#{j['iter']}")
print(f"windows: {a} {len(A)}, {b} {len(B)}; statements {len(names)}")
det = ("GPU_SCAN", "FILTER", "HASH_JOIN", "DYNAMIC_FILTER", "PROJECTION")
nd = 0; ndf = 0; nact = 0; nheld = 0; nwin_df = 0
for n, (x, y) in enumerate(zip(A, B)):
    dx = {k: v for k, v in x["o"].items() if k.startswith(det)}; dy = {k: v for k, v in y["o"].items() if k.startswith(det)}
    fx = {k: (x["i"][k], x["o"][k]) for k in x["i"] if k.startswith("DYNAMIC_FILTER")}; fy = {k: (y["i"][k], y["o"][k]) for k in y["i"] if k.startswith("DYNAMIC_FILTER")}
    nact += y["eager"] - y["held"]; nheld += y["held"]; nwin_df += bool(fx)
    if x["keys"] != y["keys"] or dx != dy or fx != fy or x["ok"] != y["ok"]:
        nd += 1
        ks = sorted(k for k in set(dx) | set(dy) if dx.get(k) != dy.get(k))
        print(f"DIFF window {n} ({names[n] if len(names) == len(A) and n < len(names) else '?'}): keys equal {x['keys'] == y['keys']}; DYNAMIC_FILTER in/out {fx} vs {fy}; " + "; ".join(f"{k} {dx.get(k)}/{dy.get(k)}" for k in ks[:6]))
print(f"compared {min(len(A), len(B))} windows ({nwin_df} with a DYNAMIC_FILTER operator): {nd} differ; arm eager activations {nact}, held {nheld}")
