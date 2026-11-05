import csv,sys
sys.path.insert(0,'.')
def tim(d):
    import collections
    t=collections.defaultdict(list)
    for r in csv.DictReader(open(d+'/timings.csv')):
        if int(r['iter'])>=2: t[int(r['query'])].append(float(r['seconds']))
    return {q:sum(v)/len(v) for q,v in t.items()}
B='/home/ubuntu/sirius-scaling-20261105/00-baseline/'
T1=tim(B+'g1_a'); T4=tim(B+'g4_a')
g1={int(r['query']):r for r in csv.DictReader(open('g1_link.csv'))}
g4={int(r['query']):r for r in csv.DictReader(open('g4_link.csv'))}
f=lambda r,k: float(r.get(k) or 0)
SW=52.0  # per-switch aggregate H2D GB/s (microbench {0,1}=52; {0..3}=105)
G1=52.0
print("q   T1    T4  ideal  Ls4  La4  Lg1 | hw  stg | sw0rate sw1rate (GB/s while sw busy) | g1util g4util_sw | gap")
S=dict(T1=0,T4=0,ideal=0,Ls=0,La=0,Lg1=0,hw=0,stg=0,gap=0, floor_hw=0, floor_all=0)
rows=[]
for q in range(1,23):
    a,b=g1[q],g4[q]
    scan=lambda r,d: f(r,f'h2d_dev{d}')  # all h2d per device
    # per-switch scan-only bytes: scale per-device h2d by scan share
    sf=(f(b,'h2d')-f(b,'h2d_staging'))/f(b,'h2d')
    sw0=f(b,'h2d_dev0')+f(b,'h2d_dev1'); sw1=f(b,'h2d_dev2')+f(b,'h2d_dev3')
    La=max(sw0,sw1)/SW/1e9; Ls=La*sf
    Lg1=f(a,'h2d')/G1/1e9
    ideal=T1[q]/4
    hw=min(max(0,Ls-ideal),T4[q]-ideal)
    stg=min(max(0,La-max(ideal,Ls)),T4[q]-ideal-hw)
    r0=sw0/1e6/f(b,'busy_sw0_ms'); r1=sw1/1e6/f(b,'busy_sw1_ms')
    u1=Lg1/(f(a,'wall_ms')/1e3); u4=La/(f(b,'wall_ms')/1e3)
    gap=T4[q]-ideal
    print(f"{q:2d} {T1[q]:5.2f} {T4[q]:5.2f} {ideal:5.2f} {Ls:5.2f} {La:5.2f} {Lg1:5.2f} | {hw:4.2f} {stg:4.2f} | {r0:5.1f} {r1:5.1f} | {u1*100:4.0f}% {u4*100:4.0f}% | {gap:5.2f}")
    for k,v in dict(T1=T1[q],T4=T4[q],ideal=ideal,Ls=Ls,La=La,Lg1=Lg1,hw=hw,stg=stg,gap=gap,floor_hw=max(ideal,Ls),floor_all=max(ideal,La)).items(): S[k]+=v
    rows.append(dict(query=q,T1=T1[q],T4=T4[q],ideal=ideal,link_floor_scan=Ls,link_floor_all=La,link_floor_g1=Lg1,hw_share=hw,staging_share=stg,gap=gap,g1_link_util=u1,g4_link_util=u4))
print({k:round(v,2) for k,v in S.items()})
w=csv.DictWriter(open('floors.csv','w'),rows[0].keys()); w.writeheader(); [w.writerow(r) for r in rows]
