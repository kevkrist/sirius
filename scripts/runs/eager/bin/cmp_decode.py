#!/usr/bin/env python3
"""cmp_decode.py <logpass dir A> <logpass dir B>: hot window (last) of each query's TRACE log; compares the per-chunk
compressed-scan decode lines: chunk count per (join_filters, status, generation, row_filtered), summed survivors / rows,
and the membership measurement lines ('join filters kept': measured chunks only)."""
import glob, re, sys, collections
RD = re.compile(r"decode (APPLIED|NOT applied \(plain output\)) \(status=(\d+) generation=(\d+)\).*join_filters=(\d+) survivors=(-?\d+)/(\d+).*row_filtered=(\w+)")
RK = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
def load(d, q):
    fs = glob.glob(f"{d}/tpch_*_q{q}/log_dir/sirius_*.log")
    if not fs: return None
    lines = open(fs[0], errors='replace').read().split('\n')
    b = [i for i, l in enumerate(lines) if '[window] begin' in l]
    if not b: return None
    hot = lines[b[-1]:]
    dec = collections.Counter(); surv = rows = 0; kept = collections.Counter()
    for l in hot:
        m = RD.search(l)
        if m:
            dec[(m.group(1)[:3], int(m.group(4)), m.group(7))] += 1
            if int(m.group(5)) >= 0: surv += int(m.group(5)); rows += int(m.group(6))
        m = RK.search(l)
        if m: kept[(m.group(3), m.group(4), m.group(5))] += 1
    return dict(dec=dict(dec), surv=surv, rows=rows, kept=dict(kept), ok=any('outcome=ok' in l for l in hot))
A, B = sys.argv[1:3]; same = diff = 0
for q in range(1, 23):
    a, b = load(A, q), load(B, q)
    if a is None or b is None: print(f"q{q}: missing"); diff += 1; continue
    eq = a['dec'] == b['dec'] and a['surv'] == b['surv'] and a['rows'] == b['rows']
    same += eq; diff += (not eq)
    partial = [k for k in b['kept'] if k[0] != k[1]]
    print(f"q{q}: decode chunks {'SAME' if eq else 'DIFF'}: {sum(a['dec'].values())} chunks, with filters {sum(v for k, v in a['dec'].items() if k[1] > 0)} -> {sum(v for k, v in b['dec'].items() if k[1] > 0)}, "
          f"survivors {a['surv']}/{a['rows']} -> {b['surv']}/{b['rows']}; measured chunks {sum(a['kept'].values())} -> {sum(b['kept'].values())}; "
          f"groups {a['kept']} -> {b['kept']}" + ("" if eq else f"  A={a['dec']} B={b['dec']}"))
print(f"{same} same, {diff} differ")
