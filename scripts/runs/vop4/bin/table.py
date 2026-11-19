#!/usr/bin/env python3
"""table.py <tag> [q...]: A/B table for runs/vop4. Hot = iterations >= 2 (iter0 warm, iter1 dropped).
Pooled median per side over all blocks; per-process medians listed in run order (by dir timestamp)."""
import csv, glob, re, statistics as st, sys, hashlib, os
B = "/opt/dlami/nvme/dfperf/runs/vop4"
tag = sys.argv[1]; qs = [int(x) for x in sys.argv[2:]] or range(1, 23)
BAD = r"execution fallback|fallback to DuckDB|falling back|Error in Sirius|bad_alloc|out of memory"
def load(side, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{tag}_{side}[0-9]*/tpch_*_q{q}")):
        t = {}
        try:
            for r in csv.DictReader(open(f"{d}/csv/runtimes.csv")):
                if r.get("query") in (f"q{q}", str(q)): t[int(r["iteration"])] = float(r["runtime_s"]) * 1000
        except Exception: continue
        hot = [t[i] for i in sorted(t) if i >= 2]
        lg = glob.glob(f"{d}/log_dir/sirius_*.log"); txt = open(lg[0], errors="replace").read() if lg else ""
        try: raw = open(f"{d}/sirius/q{q}/result.txt", "rb").read(); res = hashlib.md5(raw).hexdigest()[:8]; srt = hashlib.md5(b"\n".join(sorted(raw.split(b"\n")))).hexdigest()[:8]
        except Exception: res = srt = "none"
        if len(hot) >= 2:
            out.append(dict(hot=hot, res=res, srt=srt, bad=len(re.findall(BAD, txt)), constr=txt.count("construction started"),
                            ts=os.path.basename(d).split("_")[2], lab=d.split("/")[-2].split("_")[-1]))
    return out
print("| q | A med | B med | B-A ms | B-A % | n A/B | per-process medians in time order | A range | B range | result md5 A / B | sorted md5 A / B | constr A/B | flags |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
ta = tb = 0
for q in qs:
    A = load("A", q); Bv = load("B", q)
    if not A or not Bv: continue
    ah = [v for x in A for v in x["hot"]]; bh = [v for x in Bv for v in x["hot"]]
    am, bm = st.median(ah), st.median(bh); ta += am; tb += bm
    ra = {x["res"] for x in A}; rb = {x["res"] for x in Bv}; sa = {x["srt"] for x in A}; sb = {x["srt"] for x in Bv}
    flags = ("RESULT-DIFF " if ra != rb else "") + ("SORTED-DIFF " if sa != sb else "") + ("BAD " if any(x["bad"] for x in A + Bv) else "")
    order = sorted([(x["ts"], "A", x) for x in A] + [(x["ts"], "B", x) for x in Bv], key=lambda z: z[0])
    seq = " ".join(f"{s}{st.median(x['hot']):.0f}@{ts[:2]}:{ts[2:4]}" for ts, s, x in order)
    ca = {x["constr"] for x in A}; cb = {x["constr"] for x in Bv}
    print(f"| {q} | {am:.1f} | {bm:.1f} | {bm-am:+.1f} | {(bm/am-1)*100:+.1f} | {len(ah)}/{len(bh)} | {seq} | {min(ah):.0f}-{max(ah):.0f} | {min(bh):.0f}-{max(bh):.0f} | {','.join(sorted(ra))} / {','.join(sorted(rb))} | {','.join(sorted(sa))} / {','.join(sorted(sb))} | {sorted(ca)}/{sorted(cb)} | {flags} |")
print(f"| sum | {ta:.0f} | {tb:.0f} | {tb-ta:+.1f} | {(tb/ta-1)*100:+.2f} | | | | | | | | |")
