import sqlite3, sys, re, collections, statistics, bisect
for db in sys.argv[1:]:
    q = re.search(r'_q(\d+)', db).group(1)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    allk = c.execute("""select k.start, k.end, k.deviceId, k.gridX*k.blockX, s.value, k.streamId from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName order by k.start""").fetchall()
    bydev=collections.defaultdict(list)
    for r in allk: bydev[r[2]].append(r)
    res=[]
    for dev,ks in bydev.items():
        starts=[k[0] for k in ks]
        for i,k in enumerate(ks):
            if 'set_lookup' not in k[4] or k[3]<1e6: continue
            # overlap with other kernels on same device
            ov=0; names=collections.Counter()
            j=i-1
            lo=max(0,i-400)
            for o in ks[lo:bisect.bisect_left(starts,k[1])]:
                if o is k: continue
                a=max(o[0],k[0]); b=min(o[1],k[1])
                if b>a:
                    ov+=b-a; names[re.sub(r'<.*','',o[4])[:60]]+= b-a
            dur=k[1]-k[0]
            res.append((k[3]/dur, ov/dur, k[3], dur, names))
    res.sort(key=lambda x: x[0])
    n=len(res)
    def grp(lst,lab):
        if not lst: return
        tot=collections.Counter()
        for r in lst: tot.update(r[4])
        d=sum(r[3] for r in lst)
        print(f"  {lab}: n={len(lst)} rate={sum(r[2] for r in lst)/d:.1f}G/s  overlap_frac(med)={statistics.median(r[1] for r in lst):.2f} mean={sum(r[1]*r[3] for r in lst)/d:.2f}  top overlap: {[(k,round(v/d,2)) for k,v in tot.most_common(3)]}")
    print(f"== q{q}")
    grp(res[:n//3],"slowest third"); grp(res[n//3:2*n//3],"middle"); grp(res[2*n//3:],"fastest third")
