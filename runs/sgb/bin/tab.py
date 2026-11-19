#!/usr/bin/env python3
# tab.py <A side> <B side> [block-prefix ...] : per query medians (ms) of hot iterations >= 2 over all processes of the blocks
import sys,glob,csv,os,re,statistics as st,collections
R='/opt/dlami/nvme/dfperf/runs/sgb'; a,b=sys.argv[1:3]; blocks=sys.argv[3:]
S=collections.defaultdict(lambda: collections.defaultdict(list)); P=collections.defaultdict(lambda: collections.defaultdict(list))
for d in sorted(glob.glob(R+'/*_*_[12]')):
    m=re.match(r'(.+)_(tip|base|sgb|sgboff)_([12])$',os.path.basename(d))
    if not m or (blocks and m.group(1) not in blocks): continue
    side=m.group(2)
    for f in glob.glob(d+'/tpch_*/csv/runtimes.csv'):
        hot=[]
        for r in csv.DictReader(open(f)):
            if r['query']!='TOTAL' and int(r['iteration'])>=2: hot.append(float(r['runtime_s'])*1e3); q=r['query']
        if hot: S[q][side]+=hot; P[q][side].append(st.median(hot))
tot={a:0,b:0}
print('q | %s med (n; proc medians) | %s med (n; proc medians) | B-A ms | %%'%(a,b))
for q in sorted(S,key=lambda x:int(x[1:])):
    if not S[q][a] or not S[q][b]: continue
    ma,mb=st.median(S[q][a]),st.median(S[q][b]); tot[a]+=ma; tot[b]+=mb
    f=lambda s:'%.1f (%d; %s) sd %.1f'%(st.median(S[q][s]),len(S[q][s]),' '.join('%.0f'%x for x in P[q][s]),st.pstdev(S[q][s]))
    print('%s | %s | %s | %+.1f | %+.1f%%'%(q,f(a),f(b),mb-ma,(mb-ma)/ma*100))
print('SUM | %.0f | %.0f | %+.1f | %+.2f%%'%(tot[a],tot[b],tot[b]-tot[a],(tot[b]-tot[a])/tot[a]*100 if tot[a] else 0))
