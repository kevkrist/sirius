#!/usr/bin/env python3
"""alloc_rate.py <sqlite> <start> <end> [bin_ms] : process-wide cudaMallocFromPoolAsync+cudaFreeAsync calls per bin (k calls/s),
mean latency per call per bin; plus totals. Compare against microbench ceiling ~280-350k calls/s (driver-global)."""
import sqlite3, sys
from collections import defaultdict
db, W0, W1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); B = float(sys.argv[4]) if len(sys.argv) > 4 else 100
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
ids = [k for k, v in S.items() if v.startswith('cudaMallocFromPoolAsync') or v.startswith('cudaFreeAsync')]
nb = int((W1 - W0) / 1e6 / B) + 1; n = [0] * nb; lat = [0.0] * nb
q = f"select start,end from CUPTI_ACTIVITY_KIND_RUNTIME where nameId in ({','.join(map(str, ids))}) and start>=? and end<=?"
tot = 0; totl = 0
for s, e in c.execute(q, (W0, W1)):
    b = int((s - W0) / 1e6 / B); n[b] += 1; lat[b] += (e - s) / 1e3; tot += 1; totl += (e - s) / 1e3
print(f"total malloc+free calls {tot}  mean {totl/max(tot,1):.1f} us  window {(W1-W0)/1e6:.0f} ms  avg rate {tot/((W1-W0)/1e9)/1e3:.0f} k/s")
for i in range(nb):
    print(f"{i*B:6.0f} ms  {n[i]/(B/1e3)/1e3:6.0f} k calls/s  mean {lat[i]/max(n[i],1):6.1f} us")
