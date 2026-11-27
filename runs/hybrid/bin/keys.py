#!/usr/bin/env python3
"""keys.py <tag> <armA> <armB> [q ...]: compare DF key lines (debug logs) and results of two arms, last window of each process;
print time-to-filter-ready (ms from [window] begin to each join's [dynamic_filter_key] line), medians over hot windows."""
import glob, re, sys, hashlib, statistics as st, collections
B = "/opt/dlami/nvme/dfperf/runs/hybrid"
tag, A, Bb = sys.argv[1:4]; qs = sys.argv[4:] or [str(i) for i in range(1, 23)]
ts = lambda l: (lambda h, m, s: int(h) * 3600 + int(m) * 60 + float(s))(*l[12:24].split(":")) * 1000
def load(arm, q):
    d = glob.glob(f"{B}/{tag}_{arm}/tpch_*_q{q}")
    if not d: return None
    d = sorted(d)[-1]; wins = []; cur = None
    for f in sorted(glob.glob(d + "/log_dir/sirius_*.log")):
        for l in open(f, errors="replace"):
            if "[window] begin" in l: cur = dict(t0=ts(l), keys=[], ready={}, lines=collections.Counter(), err=0); continue
            if cur is None: continue
            if "[window] end" in l: cur["ok"] = "outcome=ok" in l; cur["wall"] = ts(l) - cur["t0"]; wins.append(cur); cur = None; continue
            m = re.search(r"\[dynamic_filter_key\] .*?join=(\d+) condition=(\d+) .*?rows=(\d+) valid=(\d+) kind=(\S+) size=(\d+) zone_map=(\d+) pushed=(\d+) reason=(\S+)", l)
            if m:
                cur["keys"].append(m.groups()); cur["ready"][int(m.group(1))] = ts(l) - cur["t0"]
            if "[error]" in l or "fallback to DuckDB" in l or "falling back" in l: cur["err"] += 1
            if "[proto_oneshot]" in l and "defers" in l: cur["lines"]["defer"] += 1
            if "[eager] join" in l and "probe activated" in l: cur["lines"]["eager"] += 1
            m = re.search(r"join filters kept (\d+)/(\d+) rows", l)
            if m: cur["lines"]["kept"] += int(m.group(1)); cur["lines"]["of"] += int(m.group(2))
    r = glob.glob(d + f"/sirius/q{q}/result.txt")
    n = sum(1 for l in open(d + "/csv/runtimes.csv") if l.startswith("sirius,q"))
    wins = wins[-n:]
    return dict(wins=wins, res=hashlib.md5(open(r[0], "rb").read()).hexdigest()[:8] if r else None)
print(f"| q | windows ok {A}/{Bb} | errors | result | key lines (sorted, hot windows) | deferred joins ({Bb}) | decode-pushdown kept/of {A} | {Bb} | join: ready ms {A} -> {Bb} |"); print("|---|" + "---|" * 8)
for q in qs:
    a, b = load(A, q), load(Bb, q)
    if not a or not b: print(f"| {q} | missing |"); continue
    ha, hb = a["wins"][1:], b["wins"][1:]
    ka = [sorted(w["keys"]) for w in ha]; kb = [sorted(w["keys"]) for w in hb]
    same = all(k == ka[0] for k in ka + kb) if ka and kb else None
    joins = sorted(set(j for w in ha + hb for j in w["ready"]))
    rd = "; ".join(f"j{j}: {st.median([w['ready'][j] for w in ha if j in w['ready']] or [float('nan')]):.0f} -> {st.median([w['ready'][j] for w in hb if j in w['ready']] or [float('nan')]):.0f}" for j in joins)
    kept = lambda ws: f"{st.median([w['lines']['kept'] for w in ws]) if ws else 0:.0f}/{st.median([w['lines']['of'] for w in ws]) if ws else 0:.0f}"
    print(f"| {q} | {sum(w['ok'] for w in a['wins'])}/{len(a['wins'])} {sum(w['ok'] for w in b['wins'])}/{len(b['wins'])} | {sum(w['err'] for w in a['wins'])}/{sum(w['err'] for w in b['wins'])} | {'identical' if a['res'] == b['res'] else 'DIFF'} | {'identical' if same else 'DIFF' if same is False else '-'} ({len(ka[0]) if ka else 0}) | {st.median([w['lines']['defer'] for w in hb]) if hb else 0:.0f} | {kept(ha)} | {kept(hb)} | {rd} |")
