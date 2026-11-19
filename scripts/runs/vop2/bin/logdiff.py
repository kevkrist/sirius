#!/usr/bin/env python3
"""logdiff.py <dirA> <dirB> [q ...]: compare the last (hot) window of each query's log between two run-label dirs.
Compares [dynamic_filter_key] lines, construction started / published counts, and the decode 'join filters kept' aggregates."""
import sys, glob, re, collections
A, Bd = sys.argv[1], sys.argv[2]; qs = [int(x) for x in sys.argv[3:]] or list(range(1, 23))
def load(base, q):
    ds = sorted(glob.glob(f"{base}/tpch_*_q{q}"))
    if not ds: return None
    ls = sorted(glob.glob(f"{ds[-1]}/log_dir/sirius_*.log"))
    if not ls: return None
    lines = open(ls[0], errors="replace").read().splitlines()
    idx = [i for i, l in enumerate(lines) if "[window] begin" in l]
    w = lines[idx[-1]:] if idx else lines
    r = dict(keys=collections.Counter(), started=0, published=0, kept=[0, 0, 0], probes=collections.Counter(), bad=0, shadow=collections.Counter(), outcome=None, windows=len(idx))
    for l in w:
        if "[dynamic_filter_key]" in l:
            m = dict(re.findall(r"(\w+)=(\S+)", l.split("[dynamic_filter_key]")[1]))
            r["keys"][tuple(m.get(k) for k in ("join", "condition", "rows", "valid", "kind", "size", "zone_map", "pushed", "reason"))] += 1
        elif "construction started" in l: r["started"] += 1
        elif "constructed filters published" in l: r["published"] += 1
        elif "join filters kept" in l:
            m = re.search(r"kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)", l)
            if m:
                r["kept"][0] += int(m.group(1)); r["kept"][1] += int(m.group(2)); r["kept"][2] += 1
                r["probes"][f"{m.group(3)}of{m.group(4)}:{m.group(5)}"] += 1
        elif "[df_proto] shadow" in l:
            m = re.search(r"join=(\d+) condition=(\d+).*decision=(\w+)", l)
            if m and m.group(3) == "skip": r["shadow"][(m.group(1), m.group(2))] += 1
        elif "[window] end" in l:
            m = re.search(r"outcome=(\w+)", l); r["outcome"] = m.group(1) if m else "?"
        if re.search(r"execution fallback|fallback to DuckDB|falling back|\[error\]|OOM", l): r["bad"] += 1
    return r
for q in qs:
    a, b = load(A, q), load(Bd, q)
    if a is None or b is None: print(f"q{q}: missing ({'A' if a is None else ''}{'B' if b is None else ''})"); continue
    ka, kb = a["keys"], b["keys"]
    only_a = sorted((ka - kb).elements(), key=str); only_b = sorted((kb - ka).elements(), key=str)
    same_kept = a["kept"][:2] == b["kept"][:2]
    print(f"q{q}: keys A={sum(ka.values())} B={sum(kb.values())} started {a['started']}/{b['started']} published {a['published']}/{b['published']} "
          f"decode-kept A={a['kept']} B={b['kept']} {'SAME' if same_kept else 'DIFF'} probes A={dict(a['probes'])} B={dict(b['probes'])} "
          f"bad {a['bad']}/{b['bad']} outcome {a['outcome']}/{b['outcome']} shadow_skip={dict(b['shadow'])}")
    for k in only_a: print(f"    only A: join={k[0]} cond={k[1]} rows={k[2]} valid={k[3]} kind={k[4]} size={k[5]} zm={k[6]} pushed={k[7]} reason={k[8]}")
    for k in only_b: print(f"    only B: join={k[0]} cond={k[1]} rows={k[2]} valid={k[3]} kind={k[4]} size={k[5]} zm={k[6]} pushed={k[7]} reason={k[8]}")
