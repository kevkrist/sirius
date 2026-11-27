#!/usr/bin/env python3
"""Verifier: per-scan filter attachment and kept rows, tip TRACE log pass vs an eager TRACE log pass (hot iteration = last window).
Usage: cmp_logs.py <tip_dir> <arm_dir> [q ...]
Compared per query: (1) [dynamic_filter_key] set (join, kind, rows, size, pushed, reason); (2) compressed-scan lines
'join filters kept X/Y rows (N of M probe(s) ran, complete=C)': histogram of (N, M, C), sum X, sum Y;
(3) rows in / out of every DYNAMIC_FILTER operator; (4) rows produced by every operator; (5) eager arm only: for every scan
pipeline with a DYNAMIC_FILTER, time of its first task minus time of the last publication line before the window's end that
precedes it... reported as: number of publications logged AFTER the first task of ANY filtered scan started later by an eager activation."""
import sys, re, glob, collections, os
ts_re = re.compile(r"^\[(\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+)\.(\d+)\]")
def ts(l):
    m = ts_re.match(l)
    if not m: return None
    h, mi, s, ms = int(m[4]), int(m[5]), int(m[6]), int(m[7]); return ((h * 60 + mi) * 60 + s) * 1000 + ms
def hot_window(path):
    lines = open(path, errors="replace").read().split("\n")
    begins = [i for i, l in enumerate(lines) if "[window] begin" in l]
    ends = [i for i, l in enumerate(lines) if "[window] end" in l]
    if not begins: return [], "no window"
    b = begins[-1]; e = [x for x in ends if x > b]
    return lines[b:(e[0] + 1 if e else len(lines))], ("ok" if e and "outcome=ok" in lines[e[0]] else "no end line")
key_re = re.compile(r"\[dynamic_filter_key\].*join=(\d+) condition=(\d+).*?rows=(\d+).*?kind=(\S+) size=(\d+) zone_map=(\d+) pushed=(\d+) reason=(\S+)")
kept_re = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
op_re = re.compile(r"Pipeline (\d+): operator (\w+) \(id=(\d+)\) task=\d+ (executing on|produced) \d+ batches, num rows: (\d+)")
def digest(path):
    w, st = hot_window(path)
    d = dict(status=st, keys=set(), kept=collections.Counter(), kx=0, ky=0, opin=collections.Counter(), opout=collections.Counter(),
             pub=[], first_task={}, eager=[], dfpipes=set())
    for l in w:
        m = key_re.search(l)
        if m: d["keys"].add(m.groups()); d["pub"].append(ts(l)); continue
        m = kept_re.search(l)
        if m: d["kept"][(m[3], m[4], m[5])] += 1; d["kx"] += int(m[1]); d["ky"] += int(m[2]); continue
        m = op_re.search(l)
        if m:
            key = f"{m[2]}({m[3]})"
            if m[4] == "produced": d["opout"][key] += int(m[5])
            else:
                d["opin"][key] += int(m[5])
                p = int(m[1]); t = ts(l)
                if p not in d["first_task"]: d["first_task"][p] = t
                if m[2] == "DYNAMIC_FILTER": d["dfpipes"].add(p)
            continue
        if "[eager] join" in l: d["eager"].append((ts(l), l.split("[eager] ")[1][:60]))
    return d
def find(dir_, q):
    c = sorted(glob.glob(f"{dir_}/tpch_*_q{q}/log_dir/*.log")); return c[-1] if c else None
tip_dir, arm_dir = sys.argv[1], sys.argv[2]
qs = [int(x) for x in sys.argv[3:]] or list(range(1, 23))
print("| q | windows | DF keys tip / arm | key set equal | kept-line classes (N of M ran, complete) equal; line counts tip / arm | kept X/Y tip | kept X/Y arm | DYNAMIC_FILTER rows in/out equal | rows PRODUCED by every operator equal (inputs of CONCAT are logged per task and vary) | filtered scan pipelines whose first task precedes a later publication (tip / arm) | min gap last publication -> first task of a filtered scan started after it, ms (tip / arm) |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
bad = 0
for q in qs:
    a, b = find(tip_dir, q), find(arm_dir, q)
    if not a or not b: continue
    A, B = digest(a), digest(b)
    keq = A["keys"] == B["keys"]; kepteq = set(A["kept"]) == set(B["kept"])
    dfa = {k: (A["opin"][k], A["opout"][k]) for k in A["opin"] if k.startswith("DYNAMIC_FILTER")}
    dfb = {k: (B["opin"][k], B["opout"][k]) for k in B["opin"] if k.startswith("DYNAMIC_FILTER")}
    alleq = A["opout"] == B["opout"]
    def early(D):
        # filtered scan pipelines that started before the LAST publication of the window (candidates, not violations:
        # a scan targeted only by earlier joins legitimately starts before later, unrelated publications)
        if not D["pub"]: return 0, None
        n = 0; gaps = []
        for p in D["dfpipes"]:
            t = D["first_task"][p]
            later = [x for x in D["pub"] if x > t]; prev = [x for x in D["pub"] if x <= t]
            if later: n += 1
            if prev: gaps.append(t - max(prev))
        return n, (min(gaps) if gaps else None)
    ea, eb = early(A), early(B)
    if not (keq and kepteq and dfa == dfb and alleq): bad += 1
    diff = ""
    if not alleq:
        ks = [k for k in set(A["opout"]) | set(B["opout"]) if A["opout"][k] != B["opout"][k]]
        diff = " diff: " + "; ".join(f"{k} in {A['opin'][k]}/{B['opin'][k]} out {A['opout'][k]}/{B['opout'][k]}" for k in sorted(ks)[:6])
    print(f"| {q} | {A['status']} / {B['status']} | {len(A['keys'])} / {len(B['keys'])} | {keq} | {kepteq} {sorted(A['kept'].items())} / {sorted(B['kept'].items())} | {A['kx']}/{A['ky']} | {B['kx']}/{B['ky']} | {dfa == dfb} ({len(dfa)} ops) | {alleq}{diff} | {ea[0]} / {eb[0]} | {ea[1]} / {eb[1]} |")
print(f"queries with any difference: {bad}")
