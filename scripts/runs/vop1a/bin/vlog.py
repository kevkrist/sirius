#!/usr/bin/env python3
"""vlog.py <runs root> <label> <armA> <armB> <q> [...] : log-pass diff tip vs proto (debug or trace logs).
Compares, over the whole process (all iterations):
  - `join filters kept X/Y rows (N of M probe(s) ran, complete=..)`: line count, (N of M, complete) split, sum X, sum Y
  - `[dynamic_filter_key]` lines (timestamp stripped) as a multiset
  - `construction started` / `constructed filters published ... N filter(s) pushed to T target(s)` as multisets
  - trace only: per operator (name, id): number of `produced` lines, sum of produced rows, sum of `executing on` rows
"""
import collections, glob, re, sys

root, label, armA, armB = sys.argv[1:5]
qs = sys.argv[5:]


def load(arm, q):
    kept = collections.Counter()
    sx = sy = n = 0
    keys = collections.Counter()
    pub = collections.Counter()
    started = collections.Counter()
    prod = collections.defaultdict(lambda: [0, 0, 0, 0])  # produced lines, produced rows, exec lines, exec rows
    other = collections.Counter()
    for d in sorted(glob.glob(f"{root}/{label}/{arm}/tpch_*_q{q}")):
        for f in glob.glob(d + "/log_dir/sirius_*.log"):
            for l in open(f, errors="replace"):
                m = re.search(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)", l)
                if m:
                    n += 1
                    sx += int(m.group(1))
                    sy += int(m.group(2))
                    kept[(m.group(3), m.group(4), m.group(5))] += 1
                    continue
                if "[dynamic_filter_key]" in l:
                    keys[l.split("[dynamic_filter_key]")[1].strip()] += 1
                    continue
                m = re.search(r"constructed filters published(?: \(([^)]*)\))?: (\d+) filter\(s\) pushed to (\d+) target", l)
                if m:
                    pub[(m.group(1), m.group(2), m.group(3))] += 1
                    continue
                m = re.search(r"construction started: (.*)$", l)
                if m:
                    started[m.group(1).strip()] += 1
                    continue
                m = re.search(r"operator (\w+) \(id=(\d+)\) task=\d+ (produced|executing on) \d+ batches, num rows: ([\d ]+),", l)
                if m:
                    rows = sum(int(x) for x in m.group(4).split())
                    p = prod[(m.group(1), int(m.group(2)))]
                    if m.group(3) == "produced":
                        p[0] += 1
                        p[1] += rows
                    else:
                        p[2] += 1
                        p[3] += rows
                    continue
                if re.search(r"execution fallback|fallback to DuckDB|falling back|\[error\]|reschedule \(retry|skipped|declined|lost|late", l):
                    if "[dynamic_filter" in l or "[error]" in l or "fallback" in l:
                        other[re.sub(r"^\[[^]]*\] ", "", l).strip()[:160]] += 1
    return dict(kept=kept, n=n, sx=sx, sy=sy, keys=keys, pub=pub, started=started, prod=dict(prod), other=other)


for q in qs:
    A, B = load(armA, q), load(armB, q)
    print(f"## q{q}  ({armA} vs {armB})")
    print(f"kept lines: {A['n']} vs {B['n']}; sum kept {A['sx']} vs {B['sx']}; sum input {A['sy']} vs {B['sy']}; split equal: {A['kept'] == B['kept']} {dict(A['kept'])}")
    print(f"[dynamic_filter_key] multiset equal: {A['keys'] == B['keys']} ({sum(A['keys'].values())} vs {sum(B['keys'].values())} lines, {len(A['keys'])} distinct)")
    print(f"construction started equal: {A['started'] == B['started']} ({sum(A['started'].values())} vs {sum(B['started'].values())}); published equal: {A['pub'] == B['pub']} ({sum(A['pub'].values())} vs {sum(B['pub'].values())}) {dict(A['pub'])}")
    if A["prod"] or B["prod"]:
        ops = sorted(set(A["prod"]) | set(B["prod"]), key=lambda k: k[1])
        diff = [(o, A["prod"].get(o), B["prod"].get(o)) for o in ops if A["prod"].get(o) != B["prod"].get(o)]
        rows_diff = [(o, a, b) for o, a, b in diff if a is None or b is None or a[1] != b[1] or a[3] != b[3]]
        print(f"trace: {len(ops)} operators; identical (lines+rows) {len(ops) - len(diff)}; differing in row sums {len(rows_diff)}; differing only in task/line count {len(diff) - len(rows_diff)}")
        for o, a, b in diff:
            print(f"   {o}: {armA} [produced lines, rows, exec lines, rows]={a} {armB}={b}")
        df = [o for o in ops if o[0] == "DYNAMIC_FILTER"]
        print(f"   DYNAMIC_FILTER ops: {len(df)}; produced rows {sum(A['prod'].get(o, [0,0,0,0])[1] for o in df)} vs {sum(B['prod'].get(o, [0,0,0,0])[1] for o in df)}; input rows {sum(A['prod'].get(o, [0,0,0,0])[3] for o in df)} vs {sum(B['prod'].get(o, [0,0,0,0])[3] for o in df)}")
    if A["other"] or B["other"]:
        print(f"other DF/error lines: {armA} {dict(A['other'])} | {armB} {dict(B['other'])}")
