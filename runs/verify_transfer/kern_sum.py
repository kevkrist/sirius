import sqlite3, sys, glob, re, collections
# per capture: add_if_n kernels grouped into "sites" by start-time clusters (gap > 100 ms) within the last iteration
for db in sorted(glob.glob(sys.argv[1])):
    q = re.search(r'_q(\d+)/', db).group(1)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = c.execute("""select k.start, k.end, k.deviceId, k.gridX*k.blockX from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName where s.value like '%add_if_n%' order by k.start""").fetchall()
    if not rows: print(f"q{q}: no add_if_n"); continue
    n = len(rows)//3; rows = rows[2*n:]   # last (hot) iteration of 3
    sites = []; cur = []
    for r in rows:
        if cur and r[0] - cur[-1][0] > 30e6: sites.append(cur); cur = []
        cur.append(r)
    sites.append(cur)
    for s in sites:
        tot_rows = sum(r[3] for r in s); gpu_ms = sum(r[1]-r[0] for r in s)/1e6
        perdev = collections.defaultdict(float)
        for r in s: perdev[r[2]] += (r[1]-r[0])/1e6
        span = (max(r[1] for r in s) - min(r[0] for r in s))/1e6
        print(f"q{q} site: kernels {len(s):3d} thr {tot_rows/1e6:8.1f}M  gpu_ms {gpu_ms:7.2f}  rate {tot_rows/gpu_ms/1e3:6.1f} Grows/s  max/dev {max(perdev.values()):6.2f}  span {span:6.2f}  batch thr min/max {min(r[3] for r in s)/1e6:.2f}/{max(r[3] for r in s)/1e6:.2f}M")
