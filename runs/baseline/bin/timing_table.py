#!/usr/bin/env python3
"""Per-query timing table from the 4 interleaved labels. Hot = iterations 1..3 (iter0 = warm)."""
import csv, glob, json, math, re, statistics as st, sys
B = "/opt/dlami/nvme/dfperf/runs/baseline"
labels = ["baseline_tip", "baseline_tip_b", "baseline_off", "baseline_off_b"]
def load(label, q):
    ds = sorted(glob.glob(f"{B}/{label}/tpch_*_q{q}"))
    if not ds: return None
    d = ds[-1]
    try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
    except Exception: return None
    t = {}
    for r in rows:
        if r.get("query") in (f"q{q}", str(q)):
            try: t[int(r["iteration"])] = float(r["runtime_s"])
            except Exception: pass
    hot = [t[i] for i in sorted(t) if i >= 1]
    if len(hot) < 3: return None
    lg = glob.glob(f"{d}/log_dir/sirius_*.log")
    txt = open(lg[0], errors="replace").read() if lg else ""
    return dict(warm=t.get(0), hot=hot, med=st.median(hot), min=min(hot), max=max(hot),
                constr=txt.count("construction started"), fallback=len(re.findall(r"execution fallback|fallback to DuckDB|falling back", txt, re.I)),
                err=len(re.findall(r"Error in Sirius|bad_alloc|out of memory|illegal memory", txt)),
                ok=txt.count("outcome=ok"), pinned=txt.count("assigned pinned entry"), retries=txt.count("reschedule (retry"))
res = {}
for q in range(1, 23):
    res[q] = {l: load(l, q) for l in labels}
json.dump(res, open(f"{B}/timing_summary.json", "w"), indent=1)
ms = lambda x: f"{x*1000:.0f}"
print("| q | tip A med (min) ms | tip B med (min) ms | A/A delta % (B vs A) | tip med ms (6 hot) | DF-off A med ms | DF-off B med ms | off A/A % | DF-off med ms (6 hot) | DF net value ms (off - tip) | off/tip | tip constr/iter | flags |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
tot = {k: 0.0 for k in ("a", "b", "tip", "offa", "offb", "off")}
logs = {k: 0.0 for k in tot}
n = 0
for q in range(1, 23):
    r = res[q]
    if any(r[l] is None for l in labels):
        print(f"| {q} | incomplete: {[l for l in labels if r[l] is None]} |"); continue
    a, b, oa, ob = (r[l] for l in labels)
    tip = st.median(a["hot"] + b["hot"]); off = st.median(oa["hot"] + ob["hot"])
    aa = (b["med"] / a["med"] - 1) * 100; oaa = (ob["med"] / oa["med"] - 1) * 100
    flags = []
    for l in labels:
        x = r[l]
        if x["fallback"] or x["err"] or x["retries"]: flags.append(f"{l}:fb={x['fallback']},err={x['err']},retry={x['retries']}")
    if oa["constr"] or ob["constr"]: flags.append("DF-off still constructs!")
    spread = max(max(a["hot"] + b["hot"]) / min(a["hot"] + b["hot"]) - 1, 0) * 100
    print(f"| {q} | {ms(a['med'])} ({ms(a['min'])}) | {ms(b['med'])} ({ms(b['min'])}) | {aa:+.1f} | {ms(tip)} | {ms(oa['med'])} | {ms(ob['med'])} | {oaa:+.1f} | {ms(off)} | {(off-tip)*1000:+.0f} | {off/tip:.2f}x | {a['constr']//4} | hot spread {spread:.0f}% {' '.join(flags)} |")
    for k, v in (("a", a["med"]), ("b", b["med"]), ("tip", tip), ("offa", oa["med"]), ("offb", ob["med"]), ("off", off)):
        tot[k] += v; logs[k] += math.log(v)
    n += 1
if n:
    g = {k: math.exp(v / n) for k, v in logs.items()}
    print(f"\nQueries complete: {n}/22")
    print(f"Totals (sum of per-query medians, s): tip A {tot['a']:.3f}, tip B {tot['b']:.3f} (A/A {100*(tot['b']/tot['a']-1):+.2f}%), tip(6 hot) {tot['tip']:.3f}; DF-off A {tot['offa']:.3f}, DF-off B {tot['offb']:.3f} (A/A {100*(tot['offb']/tot['offa']-1):+.2f}%), DF-off(6 hot) {tot['off']:.3f}; off/tip {tot['off']/tot['tip']:.3f}x")
    print(f"Geomean (ms): tip A {g['a']*1000:.1f}, tip B {g['b']*1000:.1f} (A/A {100*(g['b']/g['a']-1):+.2f}%), tip {g['tip']*1000:.1f}; DF-off A {g['offa']*1000:.1f}, DF-off B {g['offb']*1000:.1f}, DF-off {g['off']*1000:.1f}; geomean off/tip {g['off']/g['tip']:.3f}x")
