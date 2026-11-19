#!/usr/bin/env python3
"""Per-iteration DF construction cost from nsys sqlite captures (3 iterations each: iter0 warm, iter1-2 hot).
Usage: nvtx_cmp.py <label_dir> [q ...]   -> markdown rows: per query, per DF range name, hot-iteration mean of
wall ms (inclusive, same-thread), GPU kernel ms launched inside, sync-API ms inside, memcpy MB by kind inside.
HOST-staged bytes = HtoD + DtoH inside the range (excluded mechanism: reported, not attributed)."""
import sqlite3, sys, glob, json
KIND = {1: "HtoD", 2: "DtoH", 8: "DtoD", 10: "PtoP"}
UNITS = ("dynfilter::push_build_side", "dynfilter::build_bloom", "dynfilter::build_in_list", "dynfilter::build_small_in_list", "dynfilter::build_zone_map",
         "dynfilter::replicate_devices", "dynfilter::accum::begin", "dynfilter::construction::begin", "dynfilter::accum::contribute",
         "dynfilter::accum::finish", "dynfilter::accum::late_wait", "dynfilter::stage::contribute", "dynfilter::stage::gather",
         "dynfilter::bounds::contribute", "dynfilter::bounds::finish", "dynfilter::publish_hook", "sirius_physical_build_filter::execute")
def one(path):
    db = sqlite3.connect(path); c = db.cursor()
    rows = c.execute("""SELECT COALESCE(n.text, s.value) name, n.start, n.end, n.globalTid FROM NVTX_EVENTS n
      LEFT JOIN StringIds s ON s.id = n.textId WHERE n.end IS NOT NULL AND (COALESCE(n.text, s.value) LIKE 'dynfilter::%'
      OR COALESCE(n.text, s.value) LIKE 'sirius_physical_build_filter::%') ORDER BY n.start""").fetchall()
    out = {}
    for name in UNITS:
        inst = [r for r in rows if r[0] == name]
        if not inst: continue
        per = []
        for _, st, en, tid in inst:
            kn = c.execute("""SELECT COUNT(*), COALESCE(SUM(k.end-k.start),0) FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN CUPTI_ACTIVITY_KIND_KERNEL k
                 ON k.correlationId=r.correlationId WHERE r.globalTid=? AND r.start>=? AND r.end<=?""", (tid, st, en)).fetchone()
            sy = c.execute("""SELECT COALESCE(SUM(r.end-r.start),0) FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN StringIds s ON s.id=r.nameId
                 WHERE r.globalTid=? AND r.start>=? AND r.end<=? AND (s.value LIKE 'cudaStreamSynchronize%' OR s.value LIKE 'cudaEventSynchronize%'
                 OR s.value LIKE 'cudaDeviceSynchronize%')""", (tid, st, en)).fetchone()[0]
            mc = c.execute("""SELECT m.copyKind, COUNT(*), SUM(m.bytes), SUM(m.end-m.start) FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN CUPTI_ACTIVITY_KIND_MEMCPY m
                 ON m.correlationId=r.correlationId WHERE r.globalTid=? AND r.start>=? AND r.end<=? GROUP BY 1""", (tid, st, en)).fetchall()
            ms_ = c.execute("""SELECT COUNT(*), COALESCE(SUM(m.bytes),0), COALESCE(SUM(m.end-m.start),0) FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN CUPTI_ACTIVITY_KIND_MEMSET m
                 ON m.correlationId=r.correlationId WHERE r.globalTid=? AND r.start>=? AND r.end<=?""", (tid, st, en)).fetchone()
            per.append(dict(wall=(en - st) / 1e6, nk=kn[0], gpu=kn[1] / 1e6, sync=sy / 1e6,
                            mc={KIND.get(k, str(k)): (n, b / 2**20, d / 1e6) for k, n, b, d in mc}, memset=(ms_[0], ms_[1] / 2**20, ms_[2] / 1e6), start=st))
        n = len(per)
        iters = [per[i * (n // 3):(i + 1) * (n // 3)] for i in range(3)] if n % 3 == 0 else None
        out[name] = dict(n=n, per=per, iters=iters)
    return out
def agg(lst):
    d = dict(n=len(lst), wall=sum(x["wall"] for x in lst), gpu=sum(x["gpu"] for x in lst), sync=sum(x["sync"] for x in lst), nk=sum(x["nk"] for x in lst),
             memset_mb=sum(x["memset"][1] for x in lst), mc={})
    for x in lst:
        for k, (n, mb, ms) in x["mc"].items():
            a = d["mc"].setdefault(k, [0, 0.0, 0.0]); a[0] += n; a[1] += mb; a[2] += ms
    return d
label = sys.argv[1]; qs = sys.argv[2:] or [str(q) for q in range(1, 23)]
res = {}
print("| q | range | n/iter | hot wall ms/iter (iter1, iter2) | GPU kernel ms/iter | sync ms/iter | memset MB/iter | memcpy MB/iter by kind (copy ms) | warm iter0 wall ms |")
print("|---|---|---|---|---|---|---|---|---|")
for q in qs:
    fs = sorted(glob.glob(f"{label}/tpch_*_q{q}/sirius/q{q}/nsys.sqlite"))
    if not fs: continue
    o = one(fs[-1]); res[q] = {}
    for name, v in o.items():
        short = name.replace("dynfilter::", "")
        if v["iters"]:
            a0, a1, a2 = (agg(x) for x in v["iters"])
            hot = agg(v["iters"][1] + v["iters"][2])
            mc = "; ".join(f"{k} {m[1]/2:.0f} ({m[2]/2:.1f})" for k, m in sorted(hot["mc"].items())) or "-"
            print(f"| {q} | {short} | {a1['n']} | {hot['wall']/2:.2f} ({a1['wall']:.2f}, {a2['wall']:.2f}) | {hot['gpu']/2:.2f} | {hot['sync']/2:.2f} | {hot['memset_mb']/2:.0f} | {mc} | {a0['wall']:.2f} |")
            res[q][short] = dict(n=a1["n"], wall=hot["wall"] / 2, wall_iters=[a0["wall"], a1["wall"], a2["wall"]], gpu=hot["gpu"] / 2, sync=hot["sync"] / 2,
                                 memset_mb=hot["memset_mb"] / 2, mc={k: [m[0] / 2, m[1] / 2, m[2] / 2] for k, m in hot["mc"].items()},
                                 inst=[dict(wall=round(x["wall"], 2), gpu=round(x["gpu"], 2), sync=round(x["sync"], 2), mb=round(sum(m[1] for m in x["mc"].values()), 1)) for x in v["iters"][1] + v["iters"][2]])
        else:
            a = agg(v["per"])
            print(f"| {q} | {short} | {a['n']}/3 (uneven) | {a['wall']/3:.2f} (all-iter mean) | {a['gpu']/3:.2f} | {a['sync']/3:.2f} | {a['memset_mb']/3:.0f} | - | - |")
            res[q][short] = dict(n=a["n"] / 3, wall=a["wall"] / 3, gpu=a["gpu"] / 3, sync=a["sync"] / 3, uneven=True)
json.dump(res, open(f"{label}/nvtx_df.json", "w"), indent=1)
