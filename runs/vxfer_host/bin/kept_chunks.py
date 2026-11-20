#!/usr/bin/env python3
"""kept_chunks.py <bench A> <bench U>: per compressed-scan chunk 'join filters kept X/Y' lines as multisets per iteration, with the GPU of each line.
Equal multisets between arms and between iterations (where chunks land on other GPUs) => every GPU's replicas keep the same rows per chunk."""
import re, sys, glob, collections
R = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
G = re.compile(r"\[GPU:(\d+)\]|gpu[= ](\d+)|device[= ](\d+)", re.I)
def parse(bench):
    its = []
    for p in sorted(glob.glob(bench + "/log_dir/sirius_*.log")):
        for l in open(p, errors="replace"):
            if "QueryBegin: transparent_execution" in l: its.append([]); continue
            m = R.search(l)
            if m and its:
                g = G.search(l); gpu = next((x for x in g.groups() if x is not None), "?") if g else "?"
                its[-1].append((int(m.group(2)), int(m.group(1)), int(m.group(3)), int(m.group(4)), m.group(5), gpu))
    return its
A = parse(sys.argv[1]); U = parse(sys.argv[2])
key = lambda it: sorted(x[:5] for x in it)
print("chunks per iteration: A", [len(x) for x in A], "U", [len(x) for x in U])
ref = key(A[0])
for name, its in (("A", A), ("U", U)):
    for i, it in enumerate(its):
        print(f"  {name} iter {i}: multiset == A iter 0: {key(it) == ref}; lines per GPU {dict(collections.Counter(x[5] for x in it))}")
# does the same chunk (identified by its row total + kept) land on different GPUs across iterations/arms?
where = collections.defaultdict(set)
for its in (A, U):
    for it in its:
        for x in it: where[x[:5]].add(x[5])
multi = sum(1 for v in where.values() if len(v) > 1)
print(f"distinct (total, kept, probes) chunks: {len(where)}; seen on more than one GPU with the same kept count: {multi}; GPUs covered: {sorted(set(g for v in where.values() for g in v))}")
