#!/usr/bin/env python3
"""rep_check.py <bench U (many iterations)> [<bench reference>]: every iteration's per-chunk kept multiset equal to iteration 0 (and to the reference's iteration 0)?
Also: PROTO pipeline lines per iteration and Bloom finish lines; any error / fallback lines."""
import re, sys, glob, collections
R = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
P = re.compile(r"PROTO host pipeline schedule (\d+) .*?: (\d+) pinned HOST bytes, (\d+) device scratch bytes, (\d+) host waits")
F = re.compile(r"accumulated_bloom_builder::finish\] root GPU (\d+) reduced and replicated (\d+) key\(s\), (\d+) bytes in all, through (.+?): (\d+) contributing source\(s\), (\d+) target\(s\)")
def parse(bench):
    its = []; bad = []
    for p in sorted(glob.glob(bench + "/log_dir/sirius_*.log")):
        for l in open(p, errors="replace"):
            if "QueryBegin: transparent_execution" in l: its.append(dict(k=[], p=[], f=[])); continue
            if re.search(r"Error in Sirius|bad_alloc|out of memory|illegal memory|execution fallback|fallback to DuckDB|falling back|\[error\]|\[critical\]", l): bad.append(l.strip()[:200])
            if not its: continue
            m = R.search(l)
            if m: its[-1]["k"].append(tuple(int(m.group(i)) for i in (2, 1, 3, 4)) + (m.group(5),)); continue
            m = P.search(l)
            if m: its[-1]["p"].append(tuple(int(m.group(i)) for i in (1, 2, 3, 4))); continue
            m = F.search(l)
            if m: its[-1]["f"].append((int(m.group(3)), int(m.group(5)), int(m.group(6)), int(m.group(1))))
    return its, bad
U, bad = parse(sys.argv[1])
ref = sorted(U[0]["k"]); refname = "own iteration 0"
if len(sys.argv) > 2:
    A, _ = parse(sys.argv[2]); ref = sorted(A[0]["k"]); refname = "tip iteration 0"
same = [sorted(it["k"]) == ref for it in U]
print(f"iterations {len(U)}; chunks/iter {sorted(set(len(it['k']) for it in U))}; kept multiset == {refname}: {sum(same)}/{len(same)}; kept rows iter0 {sum(k[1] for k in U[0]['k'])}/{sum(k[0] for k in U[0]['k'])}")
print("  PROTO pipelines per iteration (schedule, pinned MiB):", collections.Counter(tuple(sorted((p[0], p[1] >> 20) for p in it["p"])) for it in U))
print("  finishes per iteration (MiB, sources, targets):", collections.Counter(tuple(sorted((f[0] >> 20, f[1], f[2]) for f in it["f"])) for it in U))
print("  roots used:", collections.Counter(f[3] for it in U for f in it["f"]))
print("  error/fallback lines:", len(bad), bad[:3])
