import json,csv
P=json.load(open('price_g4.json'))
def hot(run):
    t={}
    for r in csv.DictReader(open(f'../../{run}/timings.csv')):
        if r['query'].isdigit() and int(r['iter'])>=2: t.setdefault(int(r['query']),[]).append(float(r['seconds']))
    return {q:sum(v)/len(v) for q,v in t.items()}
g4=hot('00-baseline/g4_a'); g1=hot('00-baseline/g1_a'); b4=hot('20-experiments/x_g4_batch4g'); b1=hot('20-experiments/x_g1_batch4g'); t8=hot('20-experiments/x_g4_thr8')
cols=['alloc_g1_gate85','alloc_g1_soft','alloc_g1_nogate','alloc_sub_soft','h1_soft','both_soft','bothsub_soft','clone_soft']
print(f"{'q':>3}{'win':>6}{'g4':>6}{'act':>5}{'lnk85':>6}{'asat':>5}{'floor':>6}"+''.join(f'{c[:12]:>13}' for c in cols)+f"{'thrA_s':>8}{'d_b4g':>7}{'d_b4g_x':>8}{'d_thr8':>7}")
S={c:0 for c in cols}; ta=0; sd=0; sdx=0
for q in sorted(P,key=int):
    p=P[q]; qq=int(q)
    db=g4[qq]-b4[qq]; dbx=db-(g1[qq]-b1[qq])*g4[qq]/g1[qq]
    print(f"{q:>3}{p['win']/1e9:6.2f}{g4[qq]:6.2f}{p['act_mean']:5.1f}{p['linkbound_frac85']:6.2f}{p['alloc_sat_bins_frac']:5.2f}{p['h2d_bytes']/105e9:6.2f}"+''.join(f"{p[c]/1e6:13.0f}" for c in cols)+f"{p['alloc_g1_thread']/1e9:8.2f}{db*1e3:7.0f}{dbx*1e3:8.0f}{(t8[qq]-g4[qq])*1e3:7.0f}")
    for c in cols: S[c]+=p[c]
    ta+=p['alloc_g1_thread']; sd+=db; sdx+=dbx
print('SUM', ''.join(f"{c}={S[c]/1e9:.2f} " for c in cols), f'alloc_thread_excess={ta/1e9:.1f}s', f'd_batch4g={sd:.2f} d_batch4g_scaling_specific={sdx:.2f}')
print('profiled window sum', sum(P[q]['win'] for q in P)/1e9, 'unprofiled g4', sum(g4.values()), 'link floor sum', sum(P[q]['h2d_bytes'] for q in P)/105e9)
