import csv,collections
R='/home/ubuntu/sirius-scaling-20261105/'
def tim(d,it=(2,3,4)):
    t=collections.defaultdict(list)
    for r in csv.DictReader(open(R+d+'/timings.csv')):
        try: q=int(r['query']); i=int(r['iter'])
        except: continue
        if i in it: t[q].append(float(r['seconds']))
    return {q:sum(v)/len(v) for q,v in t.items()}
L={(r['run'],int(r['q'])):r for r in csv.DictReader(open('link.csv'))}
T1=tim('00-baseline/g1_a'); T1b=tim('00-baseline/g1_b'); T4=tim('00-baseline/g4_a')
T1_34=tim('00-baseline/g1_a',(3,4))
P=tim('20-experiments/x_g4_gpupin'); B4=tim('20-experiments/x_g4_batch4g'); B1=tim('20-experiments/x_g1_batch4g')
BS4=tim('20-experiments/x_g4_best'); BS1=tim('20-experiments/x_g1_best')
def sw(q): r=L[('nsys2_g4',q)]; d2h=float(r['d2h_GB']); h=float(r['h2d_GB']); f=(h-d2h)/h
# scan bytes on busier switch: scale each switch by scan share
def swscan(q):
    r=L[('nsys2_g4',q)]; h=float(r['h2d_GB']); s=float(r['scan_est_GB'])
    return max(float(r['sw0_GB']),float(r['sw1_GB']))*s/h
def floor(Tg1,Tg4,rate=52.0,qs=range(1,23)):
    tot=0; per={}
    for q in qs:
        f=swscan(q)/rate; ideal=Tg1[q]/4
        v=max(0,min(f,Tg4[q])-ideal); per[q]=v; tot+=v
    return tot,per
gap=lambda a,b,qs=range(1,23): sum(b[q]-a[q]/4 for q in qs)
print("claim floor (52 GB/s/switch, g1_a):", round(floor(T1,T4)[0],3), "gap", round(gap(T1,T4),3))
for rate in (54.0,55.0):
    print(f"floor at {rate} GB/s/switch (measured g4 busy rate):", round(floor(T1,T4,rate)[0],3))
print("floor with g1_b:", round(floor(T1b,T4)[0],3))
print("floor with g1_a iters 3-4 only (Q13 warm):", round(floor(T1_34,T4)[0],3), "Q13 T1 2-4 vs 3-4", round(T1[13],3), round(T1_34[13],3))
# per-switch busy rates
for run in ('nsys2_g1','nsys2_g4'):
    b=sum(max(float(L[(run,q)]['busy0']),float(L[(run,q)]['busy1'])) for q in range(1,23))
    by=sum(max(float(L[(run,q)]['sw0_GB']),float(L[(run,q)]['sw1_GB'])) for q in range(1,23))
    w=sum(float(L[(run,q)]['wall']) for q in range(1,23))
    print(run,"busier-switch busy s",round(b,2),"bytes",round(by,1),"rate while busy",round(by/b,1),"wall",round(w,2),"busy frac",round(b/w,3))
# batch4g / best
print("batch4g floor:", round(floor(B1,B4)[0],3), "gap", round(gap(B1,B4),3))
print("best floor:", round(floor(BS1,BS4)[0],3), "gap", round(gap(BS1,BS4),3))
# gpupin, clean 19
clean=[q for q in range(1,23) if q not in (1,3,18)]
sv={q:T4[q]-P[q] for q in clean}
print("gpupin saved clean:", round(sum(sv.values()),3), "g4",round(sum(T4[q] for q in clean),3),"gpupin",round(sum(P[q] for q in clean),3))
cap=sum(max(0,min(sv[q],T4[q]-T1[q]/4)) for q in clean)
print("gpupin saved capped at per-query gap (gap-commensurable):", round(cap,3))
below=[(q,round(P[q],3),round(T1[q]/4,3)) for q in clean if P[q]<T1[q]/4]
print("gpupin below T1/4:",below, "excess below ideal", round(sum(T1[q]/4-P[q] for q in clean if P[q]<T1[q]/4),3))
fl=floor(T1,T4)[1]
print("floor on clean:", round(sum(fl[q] for q in clean),3))
# per-query: floor vs gpupin saved vs gap
print("q floor saved gap  T4  P  T1/4")
for q in range(1,23): print(q, round(fl[q],3), round(T4[q]-P[q],3), round(T4[q]-T1[q]/4,3), round(T4[q],3), round(P[q],3), round(T1[q]/4,3))
# per-GPU uplink counterfactual: T4' = max(T1/4, T4 - s_q * scancopy4_q/2), s_q = clip(saved/scancopy,0,1) (clean); 0.40 for excluded
tot=0; tot_nc=0
for q in range(1,23):
    r=L[('nsys2_g4',q)]; busy=max(float(r['busy0']),float(r['busy1']))*float(r['scan_est_GB'])/float(r['h2d_GB'])*0.92
    s=min(1,max(0,(T4[q]-P[q])/busy)) if q in clean else 0.40
    new=max(T1[q]/4, T4[q]-s*busy/2)
    tot+=T4[q]-new
print("per-GPU-uplink lever (gpupin-sensitivity model, halve link time, floor at T1/4):", round(tot,3))
# g2x->g4 link bound
lb=[2,11,17,19,20]
G2x=tim('00-baseline/g2x_a'); G2s=tim('00-baseline/g2s_a')
print("link-bound g2x/g4:", round(sum(G2x[q] for q in lb),3), round(sum(T4[q] for q in lb),3), "g2s",round(sum(G2s[q] for q in lb),3))
print("suite g2s g2x", round(sum(G2s.values()),2), round(sum(G2x.values()),2))
print("--- variants")
T1w=dict(T1); T1w[13]=T1_34[13]
for rate in (52.0,52.5,54.0,55.0):
    print(rate, "floor g1_a", round(floor(T1,T4,rate)[0],3), "floor Q13-warm", round(floor(T1w,T4,rate)[0],3), "batch4g", round(floor(B1,B4,rate)[0],3))
# per-query observed per-switch rate on busier switch
print("q rate_g4_busier_switch rate_g1")
for q in (2,6,8,11,12,17,19,20,13,21,9,5,7):
    r=L[('nsys2_g4',q)]; a=L[('nsys2_g1',q)]
    i=0 if float(r['sw0_GB'])>=float(r['sw1_GB']) else 1
    print(q, round(float(r[f'sw{i}_GB'])/float(r[f'busy{i}']),1), round(float(a['h2d_GB'])/float(a['busy0']),1))
