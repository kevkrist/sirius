import sqlite3, sys, glob, re, collections
for db in sorted(glob.glob(sys.argv[1]), key=lambda p:int(re.search(r'_q(\d+)/',p).group(1))):
    q = re.search(r'_q(\d+)/', db).group(1)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = c.execute("""select k.start, k.end, k.deviceId, k.gridX*k.blockX, s.value from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName where s.value like '%membership_probe_functor%' order by k.start""").fetchall()
    ins = c.execute("""select sum(k.end-k.start)/1e6 from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName where s.value like '%add_if_n%'""").fetchone()[0] or 0
    allk = c.execute("select sum(end-start)/1e6 from CUPTI_ACTIVITY_KIND_KERNEL").fetchone()[0]
    if not rows: print(f"q{q}: no probe kernels; insert {ins/3:.1f}"); continue
    # split iterations by biggest gaps: use 3 equal-count groups
    n = len(rows)//3; it = rows[2*n:]
    def kind(name):
        m = re.search(r'integral_probe_adapter<(\w+), (\w+)>', name); k = f"{m.group(1)}->{m.group(2)}" if m else "other"
        f = re.search(r'sirius::op::detail::(\w+_lookup|\w+)<', name[name.find('adapter'):] ) 
        l = re.search(r'(bloom_lookup|\w+_lookup)', name)
        return k + ":" + (l.group(1) if l else "?")
    tot = sum(r[1]-r[0] for r in it)/1e6; thr = sum(r[3] for r in it)
    perdev = collections.defaultdict(float)
    for r in it: perdev[r[2]] += (r[1]-r[0])/1e6
    print(f"q{q}: probe kernels/iter {len(it):4d} rows {thr/1e6:8.0f}M gpu_ms {tot:7.1f} (avg rate {thr/tot/1e6:6.1f} G/s) max/dev {max(perdev.values()):6.1f} | insert gpu_ms/iter {ins/3:6.1f} | all kernels/iter {allk/3:7.0f}")
    # clusters by time gap > 20 ms
    cl=[]; cur=[]
    for r in it:
        if cur and r[0]-max(x[1] for x in cur) > 20e6: cl.append(cur); cur=[]
        cur.append(r)
    cl.append(cur)
    for s in cl:
        t = sum(r[1]-r[0] for r in s)/1e6; th = sum(r[3] for r in s)
        pd = collections.defaultdict(float)
        for r in s: pd[r[2]] += (r[1]-r[0])/1e6
        kinds = collections.Counter(kind(r[4]) for r in s)
        if t < 0.5: continue
        print(f"     cluster: n {len(s):3d} rows {th/1e6:8.0f}M gpu_ms {t:7.1f} rate {th/t/1e6:6.1f} G/s max/dev {max(pd.values()):6.1f} span {(max(r[1] for r in s)-min(r[0] for r in s))/1e6:7.1f} {dict(kinds)}")
