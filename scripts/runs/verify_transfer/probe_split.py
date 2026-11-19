import sqlite3, sys, glob, re, collections
seen=set()
tot=collections.defaultdict(float)
for pat in sys.argv[1:]:
  for db in sorted(glob.glob(pat), key=lambda p:int(re.search(r'_q(\d+)/',p).group(1))):
    q = re.search(r'_q(\d+)/', db).group(1)
    if q in seen: continue
    seen.add(q)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = c.execute("""select k.start, k.end, k.deviceId, k.gridX*k.blockX, s.value from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName where s.value like '%membership_probe_functor%' order by k.start""").fetchall()
    out=[f"q{q:>2}"]
    for kind in ("bloom_lookup","set_lookup","needle_lookup"):
        it=[r for r in rows if kind in r[4]]
        ms=sum(r[1]-r[0] for r in it)/1e6/3; thr=sum(r[3] for r in it)/3
        pd=collections.defaultdict(float)
        for r in it: pd[r[2]]+=(r[1]-r[0])/1e6/3
        mx=max(pd.values()) if pd else 0
        tot[kind+"_gpu"]+=ms; tot[kind+"_maxdev"]+=mx
        out.append(f"{kind}: rows/iter {thr/1e6:6.0f}M gpu_ms {ms:6.1f} max/dev {mx:5.1f} rate {(thr/ms/1e6 if ms else 0):5.1f}G/s")
    ins = c.execute("""select k.deviceId, sum(k.end-k.start)/1e6/3 from CUPTI_ACTIVITY_KIND_KERNEL k
      join StringIds s on s.id=k.demangledName where s.value like '%add_if_n%' group by 1""").fetchall()
    out.append(f"insert gpu_ms {sum(x[1] for x in ins):6.1f} max/dev {max([x[1] for x in ins] or [0]):5.1f}")
    tot["ins_gpu"]+=sum(x[1] for x in ins); tot["ins_maxdev"]+=max([x[1] for x in ins] or [0])
    print(" | ".join(out))
print({k:round(v,1) for k,v in tot.items()})
