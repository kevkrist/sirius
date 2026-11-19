# dfsum.py <nsys.sqlite>...: per capture, per iteration means: NVTX range sums (dynfilter::*), insert kernel ms, probe kernel ms by kind
# (rows = 2 x threads: cub ForPolicy{256,2}), probe max per-device ms.
import sqlite3, sys, re, collections
for db in sys.argv[1:]:
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    S = dict(c.execute('select id,value from StringIds'))
    nq = sum(1 for t,tid in c.execute("select text,textId from NVTX_EVENTS") if (t or S.get(tid))=='sirius::query') or 3
    rng = collections.defaultdict(float); cnt=collections.Counter()
    for t,tid,s,e in c.execute("select text,textId,start,end from NVTX_EVENTS where end is not null"):
        n = t or S.get(tid) or ''
        if n.startswith('dynfilter::'): rng[n]+= (e-s)/1e6; cnt[n]+=1
    ks = c.execute("select k.end-k.start, k.deviceId, k.gridX*k.blockX, s.value from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.demangledName where s.value like '%membership_probe_functor%' or s.value like '%add_if_n%'").fetchall()
    agg=collections.defaultdict(lambda:[0,0,collections.defaultdict(float)])
    for d,dev,thr,n in ks:
        kind = 'insert' if 'add_if_n' in n else ('bloom' if 'bloom_lookup' in n else 'set' if 'set_lookup' in n else 'needle')
        a=agg[kind]; a[0]+=d/1e6; a[1]+=thr*(1 if kind=='insert' else 2); a[2][dev]+=d/1e6
    print(f"== {re.search(r'(op4|base|profile)/([^/]+)/.*_q(\d+)/', db).group(2,3)} iterations={nq}")
    print("   ranges/iter: "+"; ".join(f"{k.replace('dynfilter::','')} {v/nq:.1f} ms (n={cnt[k]//nq})" for k,v in sorted(rng.items(), key=lambda x:-x[1])[:7]))
    for k,(ms,rows,pd) in agg.items():
        print(f"   {k:6s}: GPU ms/iter {ms/nq:7.1f}  max/dev {max(pd.values())/nq:6.1f}  rows/iter {rows/nq/1e6:8.0f}M  rate {rows/ms/1e6 if ms else 0:6.1f} G rows/s")
