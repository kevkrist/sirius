import sqlite3, sys, re, collections
def merge(iv):
    iv=sorted(iv); out=[]
    for a,b in iv:
        if out and a<=out[-1][1]: out[-1][1]=max(out[-1][1],b)
        else: out.append([a,b])
    return out
def tot(m): return sum(b-a for a,b in m)
def inter(A,B):
    i=j=0; t=0
    while i<len(A) and j<len(B):
        a=max(A[i][0],B[j][0]); b=min(A[i][1],B[j][1])
        if b>a: t+=b-a
        if A[i][1]<B[j][1]: i+=1
        else: j+=1
    return t
for db in sys.argv[1:]:
    q = re.search(r'_q(\d+)', db).group(1)
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    ks = c.execute("""select k.start, k.end, k.deviceId, s.value from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.demangledName""").fetchall()
    ms = c.execute("select start,end,deviceId,copyKind,bytes from CUPTI_ACTIVITY_KIND_MEMCPY").fetchall()
    print(f"== q{q}")
    for dev in sorted(set(k[2] for k in ks)):
        sl=sorted((k[0],k[1]) for k in ks if k[2]==dev and 'set_lookup' in k[3])
        if not sl: continue
        groups=[[sl[0]]]
        for iv in sl[1:]:
            if iv[0]-groups[-1][-1][1] > 150e6: groups.append([iv])
            else: groups[-1].append(iv)
        g=groups[-1]; a,b=g[0][0],g[-1][1]; w=(b-a)
        K=merge([(max(k[0],a),min(k[1],b)) for k in ks if k[2]==dev and k[1]>a and k[0]<b])
        S=merge(g)
        M=merge([(max(m[0],a),min(m[1],b)) for m in ms if m[2]==dev and m[1]>a and m[0]<b])
        byk=collections.Counter()
        for m in ms:
            if m[2]==dev and m[1]>a and m[0]<b: byk[m[3]]+=m[4]
        print(f"  dev{dev} last iter: win {w/1e6:6.1f} ms | kernels {tot(K)/w:4.0%} | memcpy {tot(M)/w:4.0%} | kern&memcpy {inter(K,M)/w:4.0%} | set_lookup {tot(S)/1e6:5.1f} ms, of which overlapped by memcpy {inter(S,M)/tot(S):4.0%} | memcpy GB by kind {dict((k,round(v/1e9,2)) for k,v in byk.items())} rate {sum(byk.values())/1e9/(tot(M)/1e9):.1f} GB/s")
