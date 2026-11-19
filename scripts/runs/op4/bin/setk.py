import sqlite3, sys, glob, re, collections, statistics
for db in sys.argv[1:]:
    q = re.search(r'_q(\d+)', db).group(1)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = c.execute("""select k.start, k.end, k.deviceId, k.gridX*k.blockX, s.value, k.gridX, k.blockX from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName where s.value like '%membership_probe_functor%' order by k.start""").fetchall()
    g=collections.defaultdict(list)
    for r in rows:
        m=re.search(r'membership_probe_functor<(.*)', r[4]); nm=m.group(1)[:230] if m else r[4][:200]
        g[nm].append(r)
    print(f"== q{q} {len(rows)} probe kernels")
    for nm,it in g.items():
        thr=sum(r[3] for r in it); ms=sum(r[1]-r[0] for r in it)/1e6
        rates=sorted(r[3]/((r[1]-r[0])/1e9)/1e9 for r in it if r[3]>1e6)
        med=statistics.median(rates) if rates else 0
        print(f"  n={len(it):4d} rows={thr/1e6:8.0f}M ms={ms:7.1f} rate={thr/ms/1e6:6.1f} med={med:6.1f} min={rates[0] if rates else 0:5.1f} max={rates[-1] if rates else 0:5.1f} blk={it[0][6]} avgrows={thr/len(it)/1e6:.1f}M\n     {nm}")
