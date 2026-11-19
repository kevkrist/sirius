#!/usr/bin/env python3
"""nsys_tab.py <q> : DF finish chain from the nsys captures of tip (ns_tip) and prototype (ns_pipe), hot iterations.
Runs nsys_df.py on each sqlite (cached as json) and prints one row per (arm, range)."""
import glob, json, os, subprocess, sys
X = "/opt/dlami/nvme/dfperf/runs/xfer_d2d"
q = sys.argv[1]
arms = sys.argv[2:] or ["ns_tip", "ns_pipe"]
MB = 2 ** 20
print("| q | arm | range | n per iter | wall ms per iter [per iteration] | max ms | host syncs | sync ms | DtoH MiB | HtoD MiB | PtoP MiB | DtoD MiB | CUDA API calls |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for arm in arms:
    sq = sorted(glob.glob(f"{X}/{arm}/tpch_*_q{q}/**/*.sqlite", recursive=True))
    if not sq:
        print(f"| {q} | {arm} | no sqlite | | | | | | | | | | |"); continue
    js = sq[-1] + ".df.json"
    if not os.path.exists(js):
        subprocess.check_call([sys.executable, f"{X}/bin/nsys_df.py", sq[-1], js])
    its = json.load(open(js))
    names = sorted({n for it in its for n in it["ranges"]})
    for n in names:
        if not any(k in n for k in ("accum::finish", "stage::gather", "replicate_devices", "late_wait")): continue
        rs = [it["ranges"].get(n) for it in its]
        rs = [r for r in rs if r]
        k = len(its)
        def mc(kind): return sum(r["mc"].get(kind, [0, 0, 0])[1] for r in rs) / k / MB
        walls = [it["ranges"][n]["wall"] if n in it["ranges"] else 0 for it in its]
        print(f"| {q} | {arm} | {n.replace('dynfilter::','')} | {sum(r['n'] for r in rs)/k:.0f} | {sum(walls)/k:.1f} [{', '.join(f'{w:.1f}' for w in walls)}] | "
              f"{max(r['max_wall'] for r in rs):.1f} | {sum(r['n_sync'] for r in rs)/k:.0f} | {sum(r['sync_ms'] for r in rs)/k:.1f} | "
              f"{mc('DtoH'):.0f} | {mc('HtoD'):.0f} | {mc('PtoP'):.0f} | {mc('DtoD'):.0f} | {sum(r['n_api'] for r in rs)/k:.0f} |")
    print(f"| {q} | {arm} | query wall (nsys) | | {sum(it['wall'] for it in its)/k:.0f} [{', '.join(f'{it['wall']:.0f}' for it in its)}] | | | | | | | | |")
