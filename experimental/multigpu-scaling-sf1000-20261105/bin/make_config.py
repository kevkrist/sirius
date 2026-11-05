#!/usr/bin/env python3
"""make_config.py <out.yaml> <telemetry_dir> <gpu_ids comma list> [key=value ...] -- derive a run config from specific_config.yaml.
Overrides: dotted.path=value under sirius (e.g. operator_params.hash_partition_bytes=2GB, telemetry.enable_quent=false)."""
import sys, yaml
src = "/home/ubuntu/sirius-scaling-20261105/configs/specific_config.orig.yaml"
out, tdir, gpus = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(",")]
cfg = yaml.safe_load(open(src))
s = cfg["sirius"]
s["topology"] = {"gpu_ids": gpus}
s["memory"]["host"]["capacity_bytes"] = "600GB"   # 1 NUMA node, 999 GB RAM; SF1000 host pins need far more than 100GB
s["telemetry"]["output_directory"] = tdir
for kv in sys.argv[4:]:
    k, v = kv.split("=", 1); node = s; parts = k.split(".")
    for p in parts[:-1]: node = node.setdefault(p, {})
    try: v = yaml.safe_load(v)
    except Exception: pass
    if v == '__DEL__': node.pop(parts[-1], None)
    else: node[parts[-1]] = v
yaml.safe_dump(cfg, open(out, "w"), sort_keys=False)
