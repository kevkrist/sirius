#!/usr/bin/env python3
"""kept.py <bench dir of arm A> <bench dir of arm U> : engine-level filter-content check from trace logs.
Per iteration (query window) and per DYNAMIC_FILTER operator: rows in / rows out summed over tasks, per GPU and total; compressed-scan
'join filters kept X/Y'; Bloom finish lines (bytes, sources, targets, transport); PROTO pipeline lines (schedule, pinned bytes).
If every GPU holds the same filter, totals are independent of task placement and must match between arms."""
import re, sys, glob, collections
R_OP = re.compile(r"\[GPU:(\d+)\] Pipeline (\d+): operator (\w+) \(id=(\d+)\) task=(\d+) (executing on|produced) (\d+) batches, num rows: ([\d ]*), size: (\d+) bytes")
R_KEPT = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
R_FIN = re.compile(r"accumulated_bloom_builder::finish\] root GPU (\d+) reduced and replicated (\d+) key\(s\), (\d+) bytes in all, through (.+?): (\d+) contributing source\(s\), (\d+) target\(s\)")
R_PROTO = re.compile(r"PROTO host pipeline schedule (\d+) .*?: (\d+) pinned HOST bytes, (\d+) device scratch bytes, (\d+) host waits")
R_PUB = re.compile(r"constructed filters published( \(Bloom through HOST staging\))?: (\d+) filter\(s\) pushed to (\d+) target\(s\), ([\d.]+) ms after the final contribution")
R_TS = re.compile(r"^\[(\d+-\d+-\d+) (\d+):(\d+):(\d+\.\d+)\]")
def parse(bench):
    f = sorted(glob.glob(bench + "/log_dir/sirius_*.log"))
    lines = [l for p in f for l in open(p, errors="replace")]
    its = []; cur = None
    for l in lines:
        if "QueryBegin: transparent_execution" in l:
            cur = dict(op=collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0, 0])), kept=[0, 0, 0, 0, 0], fin=[], proto=[], pub=[]); its.append(cur); continue
        if cur is None: continue
        m = R_OP.search(l)
        if m and m.group(3) == "DYNAMIC_FILTER":
            rows = sum(int(x) for x in m.group(8).split()); a = cur["op"][(int(m.group(2)), int(m.group(4)))][int(m.group(1))]
            if m.group(6) == "executing on": a[0] += rows; a[2] += 1
            else: a[1] += rows
            continue
        m = R_KEPT.search(l)
        if m:
            k = cur["kept"]; k[0] += int(m.group(1)); k[1] += int(m.group(2)); k[2] += int(m.group(3)); k[3] += int(m.group(4)); k[4] += 1; continue
        m = R_FIN.search(l)
        if m: cur["fin"].append((int(m.group(3)), int(m.group(5)), int(m.group(6)), m.group(4), int(m.group(1)))); continue
        m = R_PROTO.search(l)
        if m:
            t = R_TS.match(l); cur["proto"].append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))); continue
        m = R_PUB.search(l)
        if m: cur["pub"].append((bool(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))))
    return its
A = parse(sys.argv[1]); U = parse(sys.argv[2])
print(f"iterations: A {len(A)} U {len(U)}")
def summary(it):
    ops = {}
    for key, per in sorted(it["op"].items()):
        tin = sum(v[0] for v in per.values()); tout = sum(v[1] for v in per.values())
        ops[key] = (tin, tout, {g: (v[0], v[1], v[2]) for g, v in sorted(per.items())})
    return ops
for i in range(min(len(A), len(U))):
    a = summary(A[i]); u = summary(U[i])
    print(f"== iteration {i}")
    for key in sorted(set(a) | set(u)):
        x = a.get(key); y = u.get(key)
        same = x is not None and y is not None and x[0] == y[0] and x[1] == y[1]
        print(f"  DYNAMIC_FILTER pipeline {key[0]} id {key[1]}: A in {x[0] if x else '-'} out {x[1] if x else '-'} | U in {y[0] if y else '-'} out {y[1] if y else '-'} | {'SAME' if same else 'DIFF'}")
        for arm, v in (("A", x), ("U", y)):
            if v: print(f"     {arm} per GPU (in, out, tasks, out/in): " + "; ".join(f"g{g}: {p[0]} {p[1]} {p[2]} {p[1]/p[0]:.6f}" if p[0] else f"g{g}: 0" for g, p in v[2].items()))
    print(f"  compressed-scan join filters kept: A {A[i]['kept']} U {U[i]['kept']} {'SAME' if A[i]['kept'][:2] == U[i]['kept'][:2] else 'DIFF'}")
    print(f"  Bloom finishes (bytes, sources, targets, transport, root): A {sorted(A[i]['fin'])}")
    print(f"                                                              U {sorted(U[i]['fin'])}")
    print(f"  PROTO pipelines (schedule, pinned bytes, scratch, waits): U {sorted(U[i]['proto'])}  A {sorted(A[i]['proto'])}")
    print(f"  published (host-staged bloom, filters, targets, ms after final): A {A[i]['pub']}")
    print(f"                                                                    U {U[i]['pub']}")
