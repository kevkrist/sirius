#!/usr/bin/env python3
"""Coverage-parity diff base vs tip from parse_both.py JSONs -> markdown."""
import json, sys
b = json.load(open(sys.argv[1])); t = json.load(open(sys.argv[2]))
def fb(x):
    x = float(x)
    for u in ("B", "K", "M", "G"):
        if x < 1024 or u == "G": return f"{x:.0f}{u}" if u in "BK" else f"{x:.1f}{u}"
        x /= 1024
def attach_acc(r, tipf=()):
    """base accumulated Bloom -> filters on the multi-partition join whose rows match 16 bits/row."""
    out = []
    for a in r["acc"]:
        per_key = a["bytes_per_gpu"] / max(a["keys"], 1)
        cand = [j for j in r["joins"] if abs(2 * j["build_rows"] - per_key) <= 64 * 1024 or abs(2 * j["build_rows"] - per_key) / max(per_key, 1) < 0.01]
        jid = cand[-1]["join"] if cand else a.get("join_guess")
        hint = [f for f in tipf if (f["kind"] == "bloom" and abs(f["size"] - per_key) <= 4096) or (a.get("rows") and f["rows"] == a["rows"] and f["kind"] != "none")]
        if not cand and hint:
            jid = hint[0]["join"]; cand = [dict(join=jid, build_rows=hint[0]["rows"])]
            a = dict(a, via_tip=True)
        for k in ([h["key"] for h in hint if h["join"] == jid][:a["keys"]] or range(a["keys"])):
            out.append(dict(join=jid, key=k, kind="bloom", size=int(per_key), rows=a.get("rows") or (cand[-1]["build_rows"] if cand else 0), valid=0,
                            pushed=1 if a.get("pushed") else 0, path=f"accum@PARTITION({a['batches']}b/{a['gpus']}G)", reason="-",
                            guessed=not cand, ms=a.get("pub_ms_after_final"), ms_total=a.get("ms_start_to_pub")))
    return out
norm = lambda k: {"hash_in_list": "in_list"}.get(k, k)
tot = dict(base=0, tip=0, same=0, diff=0, base_only=0, tip_only=0)
for q in map(str, range(1, 23)):
    rb, rt = b.get(q), t.get(q)
    if not rb or not rt: print(f"\n### q{q}: missing"); continue
    fbase = rb["filters"] + attach_acc(rb, rt["filters"])
    ftip = rt["filters"]
    joins = {j["join"]: j for j in rt["joins"]}
    for j in rb["joins"]: joins.setdefault(j["join"], j)
    keys = sorted({(f["join"] if f["join"] is not None else -1, f["key"]) for f in fbase + ftip})
    print(f"\n### q{q}  (base hot window {rb['window_ms']:.0f} ms, tip {rt['window_ms']:.0f} ms; joins {len(rb['joins'])}; wired base {len(rb['wired'])} / tip {len(rt['wired'])})")
    if not keys: print("no DF keys at base or tip"); 
    else:
        print("| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |")
        print("|---|---|---|---|---|---|---|")
    for jid, k in keys:
        j = joins.get(jid, {})
        lay = f"{j.get('partitions','?')}p{' bcast' if j.get('broadcast') else ''} {j.get('jtype','')}"
        fb_ = [f for f in fbase if (f["join"] if f["join"] is not None else -1) == jid and f["key"] == k]
        ft_ = [f for f in ftip if f["join"] == jid and f["key"] == k]
        sb = "; ".join(f"{f['path']} {f['kind']} {fb(f['size'])} pushed={f['pushed']}{' (join guessed)' if f.get('guessed') or f.get('amb') else ''}" for f in fb_) or "-"
        stp = "; ".join(f"{f['kind']} {fb(f['size'])} pushed={f['pushed']} {f['reason']}" for f in ft_) or "-"
        bk = sorted((norm(f["kind"]), f["pushed"]) for f in fb_ if f["kind"] != "none"); tk = sorted((norm(f["kind"]), f["pushed"]) for f in ft_ if f["kind"] != "none")
        bp = [x for x in bk if x[1]]; tp = [x for x in tk if x[1]]
        if bp == tp:
            szb = sum(f["size"] for f in fb_); szt = sum(f["size"] for f in ft_)
            d = "same" if (not bp or abs(szb - szt) <= 0.01 * max(szb, szt, 1) or bp[0][0] in ("small_in_list", "in_list")) else f"same kind, size {fb(szb)} vs {fb(szt)}"
            tot["same"] += 1 if bp else 0
        elif bp and not tp: d = "**BASE ONLY**"; tot["base_only"] += 1
        elif tp and not bp: d = "**TIP ONLY**"; tot["tip_only"] += 1
        else: d = f"**kind differs** {bp} vs {tp}"; tot["diff"] += 1
        tot["base"] += len(bp); tot["tip"] += len(tp)
        print(f"| {jid} | {lay} | {j.get('build_rows','?')} | {k} | {sb} | {stp} | {d} |")
    for name, r in (("base", rb), ("tip", rt)):
        if r["declines"]:
            for s, n in r["declines"].items(): print(f"- {name} decline x{n}: {s}")
        if r.get("tip_unattached"): print(f"- {name} unattached construction events: {r['tip_unattached']}")
    def cons(r):
        s = []
        for k, v in sorted(r["kept"].items()): s.append(f"cscan probes {k}: {v[0]} chunks kept {v[1]/max(v[2],1):.4f} of {v[2]}")
        if r["ckpt"][0]: s.append(f"ckpt n={r['ckpt'][0]} {r['ckpt'][1]}->{r['ckpt'][2]}")
        for oid, o in sorted(r["ops"].items(), key=lambda x: int(x[0])):
            if o["name"] == "DYNAMIC_FILTER" and o["rows_in"]: s.append(f"DYNAMIC_FILTER({oid}) keep {o['rows_out']/o['rows_in']:.3f} of {o['rows_in']} {o['ms']:.0f}ms")
        return "; ".join(s) or "-"
    print(f"- consumer base: {cons(rb)}"); print(f"- consumer tip : {cons(rt)}")
    def opsum(r):
        d = {}
        for o in r["ops"].values(): d[o["name"]] = d.get(o["name"], 0) + o["ms"] + o["sink_ms"]
        return d
    ob, ot = opsum(rb), opsum(rt)
    print("- operator task ms (sum over tasks, exec+sink; trace run, not a timing): " + ", ".join(
        f"{n} {ob.get(n,0):.0f}->{ot.get(n,0):.0f}" for n in ("PARTITION", "CONCAT", "HASH_JOIN", "BUILD_FILTER", "DYNAMIC_FILTER", "GPU_SCAN") if ob.get(n, 0) or ot.get(n, 0)))
print("\nTOTAL pushed filters: base", tot["base"], "tip", tot["tip"], "| same", tot["same"], "kind-differs", tot["diff"], "base-only", tot["base_only"], "tip-only", tot["tip_only"])
