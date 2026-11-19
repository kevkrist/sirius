#!/usr/bin/env python3
"""Base (A,B) vs tip (A,B) per-query timing. Hot = iterations 1..3 of each fresh process; 6 hot samples per build."""
import csv, glob, json, math, re, statistics as st
R = "/opt/dlami/nvme/dfperf/runs"
SRC = {"base_a": f"{R}/base/base_a", "base_b": f"{R}/base/base_b", "tip_a": f"{R}/baseline/baseline_tip", "tip_b": f"{R}/baseline/baseline_tip_b",
       "off_a": f"{R}/baseline/baseline_dfoff", "off_b": f"{R}/baseline/baseline_dfoff_b"}
def load(d0, q):
    ds = sorted(glob.glob(f"{d0}/tpch_*_q{q}"))
    for d in reversed(ds):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {}
        for r in rows:
            try: t[int(r["iteration"])] = float(r["runtime_s"])
            except Exception: pass
        hot = [t[i] for i in sorted(t) if i >= 1]
        if len(hot) < 3: continue
        lg = glob.glob(f"{d}/log_dir/sirius_*.log"); txt = open(lg[0], errors="replace").read() if lg else ""
        return dict(warm=t.get(0), hot=hot, med=st.median(hot), min=min(hot), ok=txt.count("outcome=ok"),
                    fallback=len(re.findall("fallback", txt, re.I)), err=len(re.findall(r"Error in Sirius|bad_alloc|out of memory|illegal memory", txt)),
                    retries=txt.count("reschedule (retry"), pinned=txt.count("assigned pinned entry"), dir=d)
    return None
res = {q: {k: load(v, q) for k, v in SRC.items()} for q in range(1, 23)}
json.dump(res, open(f"{R}/base/timing_summary.json", "w"), indent=1)
ms = lambda x: f"{x*1000:.0f}"
print("| q | base med (min) ms, 6 hot | base A/B med ms | base A/A % | tip med (min) ms, 6 hot | tip A/B med ms | tip A/A % | tip/base (med) | tip/base (min) | DF-off med ms | verdict | flags |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
sb = stt = 0; lb = lt = 0; n = 0; lbm = ltm = 0
for q in range(1, 23):
    r = res[q]
    if not all(r[k] for k in ("base_a", "base_b", "tip_a", "tip_b")):
        print(f"| {q} | incomplete: {[k for k in r if not r[k]]} |"); continue
    bh = r["base_a"]["hot"] + r["base_b"]["hot"]; th = r["tip_a"]["hot"] + r["tip_b"]["hot"]
    bm, tm = st.median(bh), st.median(th)
    baa = (r["base_b"]["med"] / r["base_a"]["med"] - 1) * 100; taa = (r["tip_b"]["med"] / r["tip_a"]["med"] - 1) * 100
    ratio = tm / bm; rmin = min(th) / min(bh)
    noise = max(abs(baa), abs(taa), 2.0)
    verdict = "tip faster" if (1 - ratio) * 100 > noise and rmin < 0.99 else ("tip slower" if (ratio - 1) * 100 > noise and rmin > 1.01 else "within noise")
    off = [x for k in ("off_a", "off_b") if r[k] for x in r[k]["hot"]]
    flags = [f"{k}:fb={r[k]['fallback']},err={r[k]['err']},retry={r[k]['retries']}" for k in ("base_a", "base_b") if r[k]["err"] or r[k]["retries"]]
    print(f"| {q} | {ms(bm)} ({ms(min(bh))}) | {ms(r['base_a']['med'])}/{ms(r['base_b']['med'])} | {baa:+.1f} | {ms(tm)} ({ms(min(th))}) | {ms(r['tip_a']['med'])}/{ms(r['tip_b']['med'])} | {taa:+.1f} | {ratio:.3f} | {rmin:.3f} | {ms(st.median(off)) if off else '-'} | {verdict} | {'; '.join(flags) or '-'} |")
    sb += bm; stt += tm; lb += math.log(bm); lt += math.log(tm); lbm += math.log(min(bh)); ltm += math.log(min(th)); n += 1
if n:
    print(f"\nQueries complete: {n}/22. Sum of medians: base {sb:.3f} s, tip {stt:.3f} s (tip/base {stt/sb:.4f}). Geomean of medians: base {math.exp(lb/n)*1000:.1f} ms, tip {math.exp(lt/n)*1000:.1f} ms (ratio {math.exp((lt-lb)/n):.4f}); geomean of mins ratio {math.exp((ltm-lbm)/n):.4f}.")
