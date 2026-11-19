# For each device and each iteration: window = [first, last] set_lookup kernel; report union of ALL kernel+memcpy time on the
# device inside it (GPU busy fraction), the set_lookup share, and the window length.
import sqlite3, sys, re, collections
def union(iv):
    iv=sorted(iv); tot=0; cs=ce=None
    for a,b in iv:
        if ce is None or a>ce:
            if ce is not None: tot+=ce-cs
            cs,ce=a,b
        else: ce=max(ce,b)
    if ce is not None: tot+=ce-cs
    return tot
for db in sys.argv[1:]:
    q = re.search(r'_q(\d+)', db).group(1)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    ks = c.execute("""select k.start, k.end, k.deviceId, s.value from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.demangledName""").fetchall()
    ms = c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_MEMCPY").fetchall()
    print(f"== q{q}")
    agg=collections.defaultdict(list)
    for dev in sorted(set(k[2] for k in ks)):
        sl=sorted((k[0],k[1]) for k in ks if k[2]==dev and 'set_lookup' in k[3])
        if not sl: continue
        # split iterations by gaps > 150 ms
        groups=[[sl[0]]]
        for iv in sl[1:]:
            if iv[0]-groups[-1][-1][1] > 150e6: groups.append([iv])
            else: groups[-1].append(iv)
        for gi,g in enumerate(groups):
            a,b=g[0][0],g[-1][1]
            allk=[(max(k[0],a),min(k[1],b)) for k in ks if k[2]==dev and k[1]>a and k[0]<b]
            allm=[(max(m[0],a),min(m[1],b)) for m in ms if m[2]==dev and m[1]>a and m[0]<b]
            busy=union(allk+allm); kb=union(allk); sb=sum(y-x for x,y in g)
            agg[gi].append(((b-a)/1e6,kb/1e6,busy/1e6,sb/1e6))
    for gi,v in agg.items():
        print(f"  iter-group {gi}: "+" | ".join(f"win {w:6.1f} kern {k:6.1f} ({k/w:4.0%}) +memcpy {bz/w:4.0%} set_lookup {s:5.1f} ({s/w:4.0%})" for w,k,bz,s in v))
