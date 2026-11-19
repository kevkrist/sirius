#!/usr/bin/env python3
# tab.py <A arm> <B arm> <tag> [tag ...] : per query, hot iterations >= 2 pooled over all processes of the tags (ms): median, n, process medians, sd; B-A.
import sys,glob,csv,os,re,statistics as st,collections
R='/opt/dlami/nvme/dfperf/runs/vsgb'; a,b=sys.argv[1:3]; tags=sys.argv[3:]
S=collections.defaultdict(lambda: collections.defaultdict(list)); P=collections.defaultdict(lambda: collections.defaultdict(list))
for d in sorted(glob.glob(R+'/*_*_[0-9]')):
    m=re.match(r'(.+)_(tip|base|sgb|sgboff)_([0-9])$',os.path.basename(d))
    if not m or m.group(1) not in tags: continue
    arm=m.group(2)
    for f in glob.glob(d+'/tpch_*/csv/runtimes.csv'):
        hot=[]; q=None
        for r in csv.DictReader(open(f)):
            if r['query']!='TOTAL' and int(r['iteration'])>=2: hot.append(float(r['runtime_s'])*1e3); q=r['query']
        if hot: S[q][arm]+=hot; P[q][arm].append(st.median(hot))
tot={a:0,b:0}
print('q | %s med (n; proc medians) sd | %s med (n; proc medians) sd | B-A ms | %% | B-A of proc-median means'%(a,b))
for q in sorted(S,key=lambda x:int(re.sub(r'\D','',x))):
    if not S[q][a] or not S[q][b]: continue
    ma,mb=st.median(S[q][a]),st.median(S[q][b]); tot[a]+=ma; tot[b]+=mb
    f=lambda s:'%.1f (%d; %s) sd %.1f'%(st.median(S[q][s]),len(S[q][s]),' '.join('%.0f'%x for x in P[q][s]),st.pstdev(S[q][s]))
    print('%s | %s | %s | %+.1f | %+.1f%% | %+.1f'%(q,f(a),f(b),mb-ma,(mb-ma)/ma*100,st.mean(P[q][b])-st.mean(P[q][a])))
print('SUM | %.0f | %.0f | %+.1f | %+.2f%%'%(tot[a],tot[b],tot[b]-tot[a],(tot[b]-tot[a])/tot[a]*100 if tot[a] else 0))
