import sqlite3, sys, re
db = sys.argv[1]; pat = sys.argv[2]
c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
q = """select k.start, k.end, k.deviceId, k.streamId, k.gridX, k.blockX, s.value
from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id = k.demangledName
where s.value like ? order by k.start"""
rows = c.execute(q, (f"%{pat}%",)).fetchall()
t0 = c.execute("select min(start) from CUPTI_ACTIVITY_KIND_KERNEL").fetchone()[0]
for st, en, dev, strm, gx, bx, name in rows:
    print(f"{(st-t0)/1e6:10.1f} ms dur {(en-st)/1e6:8.3f} dev {dev} stream {strm} grid {gx} x {bx} thr={gx*bx/1e6:.2f}M {name[:60]}")
print("n", len(rows), "sum ms", sum(e-s for s,e,*_ in rows)/1e6)
