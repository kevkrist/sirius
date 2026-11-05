#!/usr/bin/env python3
"""CUDA runtime API totals in a window: count, total ms, mean us, calls/s over the window, per API.
Also the summed duty of the driver allocator calls (malloc+free) = total ms / window ms (process-wide 'busy' if serialized).
usage: apisum.py <sqlite> <start_ns> <end_ns> [top=15]"""
import sqlite3, sys
from collections import Counter
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
top = int(sys.argv[4]) if len(sys.argv) > 4 else 15
c = sqlite3.connect(db)
S = dict(c.execute("select id,value from StringIds"))
n = Counter(); t = Counter()
for nid, cnt, tot in c.execute("select nameId,count(*),sum(end-start) from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=? group by nameId", (s0, e0)):
    k = S.get(nid, str(nid)).split('_v')[0]; n[k] += cnt; t[k] += tot
W = (e0 - s0) / 1e9
print(f"window {W * 1e3:.1f} ms")
for k, v in t.most_common(top):
    print(f"  {k:32s} n={n[k]:8d} total {v / 1e6:9.1f} ms mean {v / 1e3 / n[k]:7.1f} us  rate {n[k] / W / 1e3:7.1f} k/s")
al = [k for k in t if 'MallocFromPool' in k or 'FreeAsync' in k or k.startswith('cudaMallocAsync')]
print(f"  allocator (malloc+free) n={sum(n[k] for k in al)} total {sum(t[k] for k in al) / 1e6:.1f} ms rate {sum(n[k] for k in al) / W / 1e3:.1f} k/s")
