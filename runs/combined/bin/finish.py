#!/usr/bin/env python3
"""finish.py <arm> [<arm> ...]: from the DEBUG log pass (lp_<arm>), last iteration of each process: number of DF
constructions published, of Blooms finished, Bloom bytes, and the sum of 'ms after the final contribution'
(= finish chain: cross-GPU reduce + replicate, inside the Publishing Task)."""
import glob, re, sys
B = "/opt/dlami/nvme/dfperf/runs/combined"
R_PUB = re.compile(r"constructed filters published.*?: (\d+) filter\(s\) pushed to (\d+) target\(s\), ([\d.]+) ms after the final contribution")
R_FIN = re.compile(r"accumulated_bloom_builder::finish\].*? (\d+) bytes in all, through (.+?): ")
R_START = re.compile(r"construction started: ")
ITERS = 3
def one(arm, q):
    d = sorted(glob.glob(f"{B}/lp_{arm}/tpch_*_q{q}"))
    if not d: return None
    pub, fin, start = [], [], 0
    for f in sorted(glob.glob(d[-1] + "/log_dir/sirius_*.log")):
        for line in open(f, errors="replace"):
            m = R_PUB.search(line)
            if m: pub.append(float(m.group(3))); continue
            m = R_FIN.search(line)
            if m: fin.append((int(m.group(1)), m.group(2))); continue
            if R_START.search(line): start += 1
    np_, nf = len(pub) // ITERS, len(fin) // ITERS
    lp, lf = pub[len(pub) - np_:] if np_ else [], fin[len(fin) - nf:] if nf else []
    return dict(started=start / ITERS, pubs=np_, pub_ms=sum(lp), blooms=nf, mib=sum(b for b, _ in lf) / 2**20,
                routes=",".join(sorted({r for _, r in lf})))
arms = sys.argv[1:]
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for a in arms for d in glob.glob(f"{B}/lp_{a}/tpch_*_q*")})
print("| q | " + " | ".join(f"{a}: constr / Blooms / Bloom MiB / finish ms" for a in arms) + " | route(s) in last arm |")
print("|---|" + "---|" * (len(arms) + 1))
tot = {a: [0, 0, 0.0, 0.0] for a in arms}
for q in qs:
    cells = []; r = None
    for a in arms:
        r = one(a, q)
        if r is None: cells.append("-"); continue
        cells.append(f"{r['pubs']} / {r['blooms']} / {r['mib']:.0f} / {r['pub_ms']:.1f}")
        for i, k in enumerate(("pubs", "blooms", "mib", "pub_ms")): tot[a][i] += r[k]
    print(f"| {q} | " + " | ".join(cells) + f" | {r['routes'] if r else ''} |")
print("| **sum** | " + " | ".join(f"**{t[0]} / {t[1]} / {t[2]:.0f} / {t[3]:.1f}**" for t in tot.values()) + " | |")
