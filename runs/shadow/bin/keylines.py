#!/usr/bin/env python3
"""keylines.py <labelA> <labelB> [q ...]: compare DEBUG passes under runs/shadow/<label>.
Per query: [dynamic_filter_key] lines (all iterations, as a multiset of 'join condition rows valid kind size zone_map pushed reason'),
[dynamic_filter_shadow] lines of B, result.txt md5, fallback/error line counts."""
import sys, glob, re, collections, hashlib, os
R = "/opt/dlami/nvme/dfperf/runs/shadow"
A, B = sys.argv[1], sys.argv[2]
qs = [int(x) for x in sys.argv[3:]] or list(range(1, 23))
key = re.compile(r"\[dynamic_filter_key\] v=1 sig=\w+ occ=\d+ (join=\S+ condition=\d+) build_ordinal=\d+ (rows=\d+ valid=\S+ kind=\S+ size=\d+ zone_map=\d pushed=\d+ reason=\S+)")
def load(label, q):
    ds = sorted(glob.glob(f"{R}/{label}/tpch_*_q{q}"))
    if not ds: return None
    d = ds[-1]
    text = "".join(open(f, errors="replace").read() for f in sorted(glob.glob(f"{d}/log_dir/sirius_*.log")))
    keys = collections.Counter(" ".join(m.groups()) for m in key.finditer(text))
    shadow = collections.Counter(re.sub(r"^.*\[dynamic_filter_shadow\] ", "", l) for l in text.splitlines() if "[dynamic_filter_shadow]" in l)
    bad = len(re.findall(r"execution fallback|fallback to DuckDB|falling back|\[error\]", text))
    res = f"{d}/sirius/q{q}/result.txt"
    h = hashlib.md5(open(res, "rb").read()).hexdigest()[:8] if os.path.exists(res) else None
    return keys, shadow, bad, h
for q in qs:
    a, b = load(A, q), load(B, q)
    if not a or not b: print(f"q{q}: missing ({'A' if not a else ''}{'B' if not b else ''})"); continue
    only_a = a[0] - b[0]; only_b = b[0] - a[0]
    skips = {k: n for k, n in b[1].items() if "decision=skip" in k}
    print(f"q{q}: key lines A={sum(a[0].values())} B={sum(b[0].values())} differ={sum(only_a.values())}/{sum(only_b.values())} "
          f"shadow judged={sum(b[1].values())} skip={sum(skips.values())} result={'same' if a[3] == b[3] else 'DIFF'}({a[3]},{b[3]}) bad={a[2]}/{b[2]}")
    for k, n in sorted(only_a.items()): print(f"   A only x{n}: {k}")
    for k, n in sorted(only_b.items()): print(f"   B only x{n}: {k}")
    for k, n in sorted(skips.items()): print(f"   skip x{n}: {k}")
