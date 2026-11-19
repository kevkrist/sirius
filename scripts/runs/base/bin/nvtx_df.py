#!/usr/bin/env python3
"""DF NVTX ranges from an nsys sqlite: per-instance list of push_build_side (base) / construction ranges (tip) with children."""
import sqlite3, sys
db = sqlite3.connect(sys.argv[1]); verbose = len(sys.argv) > 2
c = db.cursor()
rows = c.execute("""SELECT COALESCE(n.text, s.value) name, n.start, n.end, n.globalTid FROM NVTX_EVENTS n
 LEFT JOIN StringIds s ON s.id = n.textId WHERE COALESCE(n.text, s.value) LIKE 'dynfilter::%' ORDER BY n.start""").fetchall()
if not rows: print("no dynfilter ranges"); sys.exit()
t0 = rows[0][1]
agg = {}
for name, st, en, tid in rows:
    a = agg.setdefault(name, [0, 0.0, 0.0])
    a[0] += 1
    if en: d = (en - st) / 1e6; a[1] += d; a[2] = max(a[2], d)
print("name | n | sum ms | max ms")
for k, v in sorted(agg.items(), key=lambda x: -x[1][1]): print(f"{k} | {v[0]} | {v[1]:.2f} | {v[2]:.2f}")
tops = [r for r in rows if r[0] in ("dynfilter::push_build_side",) and r[2]]
def inside(tid, st, en, table, cols="1"):
    return c.execute(f"SELECT {cols} FROM {table} WHERE globalTid=? AND start>=? AND end<=?", (tid, st, en)).fetchall()
print("\npush_build_side instances: t(ms since first DF range) | total ms | children ms {name: ms} | memcpy inside (same thread): n, MB by kind | sync ms")
for name, st, en, tid in tops:
    ch = {}
    for n2, s2, e2, t2 in rows:
        if t2 == tid and s2 >= st and e2 and e2 <= en and n2 != name: ch[n2.replace("dynfilter::", "")] = ch.get(n2.replace("dynfilter::", ""), 0) + (e2 - s2) / 1e6
    # memcpy launched by this thread within the range
    mc = c.execute("""SELECT m.copyKind, COUNT(*), SUM(m.bytes), SUM(m.end-m.start) FROM CUPTI_ACTIVITY_KIND_RUNTIME r
       JOIN CUPTI_ACTIVITY_KIND_MEMCPY m ON m.correlationId=r.correlationId WHERE r.globalTid=? AND r.start>=? AND r.end<=? GROUP BY 1""", (tid, st, en)).fetchall()
    sy = c.execute("""SELECT SUM(r.end-r.start) FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN StringIds s ON s.id=r.nameId
       WHERE r.globalTid=? AND r.start>=? AND r.end<=? AND (s.value LIKE 'cudaStreamSynchronize%' OR s.value LIKE 'cudaEventSynchronize%' OR s.value LIKE 'cudaDeviceSynchronize%')""", (tid, st, en)).fetchone()[0] or 0
    kn = c.execute("""SELECT COUNT(*), SUM(k.end-k.start) FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN CUPTI_ACTIVITY_KIND_KERNEL k ON k.correlationId=r.correlationId
       WHERE r.globalTid=? AND r.start>=? AND r.end<=?""", (tid, st, en)).fetchone()
    mcs = "; ".join(f"kind{k}: n={n} {b/2**20:.1f}MB {d/1e6:.1f}ms" for k, n, b, d in mc)
    print(f"{(st-t0)/1e6:9.1f} | {(en-st)/1e6:8.2f} | " + ", ".join(f"{k}={v:.2f}" for k, v in ch.items()) + f" | {mcs} | sync {sy/1e6:.2f} | kernels n={kn[0]} gpu_ms={(kn[1] or 0)/1e6:.2f}")
