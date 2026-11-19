#!/usr/bin/env python3
"""rowdiff.py <dirA> <dirB> [q ...]: TRACE logs, last window: per operator (name,id) total rows executing-on and produced,
task count and summed execution ms. Prints operators whose row totals differ and a one-line verdict per query."""
import sys, glob, re, collections
A, B = sys.argv[1], sys.argv[2]; qs = [int(x) for x in sys.argv[3:]]
def load(base, q):
    ds = sorted(glob.glob(f"{base}/tpch_*_q{q}"))
    if not ds: return None
    lines = open(sorted(glob.glob(f"{ds[-1]}/log_dir/sirius_*.log"))[0], errors="replace").read().splitlines()
    idx = [i for i, l in enumerate(lines) if "[window] begin" in l]
    r = collections.defaultdict(lambda: [0, 0, 0, 0.0])  # rows_in, rows_out, tasks, ms
    for l in lines[idx[-1]:]:
        m = re.search(r"operator (\w+) \(id=(\d+)\) task=\d+ (executing on|produced) \d+ batches, num rows: ([\d ]+),", l)
        if not m: continue
        rows = sum(int(x) for x in m.group(4).split())
        k = (m.group(1), int(m.group(2)))
        if m.group(3) == "produced":
            r[k][1] += rows; r[k][2] += 1
            t = re.search(r"execution time: ([\d.]+) ms", l); r[k][3] += float(t.group(1)) if t else 0
        else: r[k][0] += rows
    return r
for q in qs:
    a, b = load(A, q), load(B, q)
    if not a or not b: print(f"q{q}: missing"); continue
    diff = [k for k in sorted(set(a) | set(b), key=lambda k: k[1]) if a.get(k, [0, 0])[:2] != b.get(k, [0, 0])[:2]]
    print(f"q{q}: {len(a)} / {len(b)} operators, row totals differ on {len(diff)}")
    for k in diff: print(f"   {k[0]}({k[1]}): A in/out {a.get(k,[0,0])[:2]} tasks {a.get(k,[0,0,0])[2]}  B in/out {b.get(k,[0,0])[:2]} tasks {b.get(k,[0,0,0])[2]}")
    for k in sorted(set(a) | set(b), key=lambda k: k[1]):
        if k[0] in ("DYNAMIC_FILTER", "PARTITION") and (abs(a.get(k,[0,0,0,0])[3] - b.get(k,[0,0,0,0])[3]) > 5):
            print(f"   time {k[0]}({k[1]}): A {a[k][3]:.1f} ms over {a[k][2]} tasks, B {b[k][3]:.1f} ms over {b[k][2]} tasks; rows in/out {a[k][:2]}")
