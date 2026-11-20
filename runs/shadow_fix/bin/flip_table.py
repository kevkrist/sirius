#!/usr/bin/env python3
"""flip_table.py <tag> <q>: medians of the forced-flip arms runs/shadow/<tag>_{tip,late,begin}{A,B}; hot = iterations >= 2."""
import csv, glob, statistics as st, sys, hashlib, os, re
B = "/opt/dlami/nvme/dfperf/runs/shadow"; tag, q = sys.argv[1], sys.argv[2]
def arm(name):
    ms, hs, started = [], set(), []
    for s in "AB":
        for d in sorted(glob.glob(f"{B}/{tag}_{name}{s}/tpch_*_q{q}")):
            t = {int(r["iteration"]): float(r["runtime_s"]) for r in csv.DictReader(open(f"{d}/csv/runtimes.csv"))}
            ms += [t[i] * 1000 for i in sorted(t) if i >= 2]
            res = f"{d}/sirius/q{q}/result.txt"
            hs.add(hashlib.md5(open(res, "rb").read()).hexdigest()[:8] if os.path.exists(res) else None)
            text = "".join(open(f, errors="replace").read() for f in glob.glob(f"{d}/log_dir/sirius_*.log"))
            started.append(len(re.findall(r"construction started", text)))
    return ms, hs, started
r = {n: arm(n) for n in ("tip", "late", "begin")}
for n, (ms, hs, started) in r.items():
    print(f"{n:6s} median {st.median(ms):8.1f} ms n={len(ms)} [{min(ms):.0f}-{max(ms):.0f}] results={sorted(map(str, hs))} construction_started/process={started}")
tip = st.median(r["tip"][0])
for n in ("late", "begin"):
    d = st.median(r[n][0]) - tip
    print(f"{n} - tip = {d:+.1f} ms ({100 * d / tip:+.2f}%)")
print(f"late saves {100 * (tip - st.median(r['late'][0])) / (tip - st.median(r['begin'][0])):.0f}% of what begin saves")
