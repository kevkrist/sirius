#!/usr/bin/env python3
# Lock-discipline audit: intervals of every recorded GPU run (start = q<N>.gpu.txt mtime, end = q<N>.stdout mtime),
# GPU memory.used at start, plus build intervals from .ninja_log output mtimes. Reports overlaps.
import glob, os, re, sys, datetime as dt
R='/opt/dlami/nvme/dfperf'
iv=[]
busy=[]
for g in glob.glob(f'{R}/runs/*/*/q*.gpu.txt'):
    so=g.replace('.gpu.txt','.stdout')
    if not os.path.exists(so): continue
    s=os.path.getmtime(g); e=os.path.getmtime(so)
    lab='/'.join(g.split('/')[-3:]).replace('.gpu.txt','')
    iv.append((s,e,lab))
    mem=[int(re.search(r'(\d+) MiB',l).group(1)) for l in open(g) if 'MiB' in l]
    if mem and max(mem)>600: busy.append((lab,mem))
def ninja(wt):
    f=f'{wt}/build/release/.ninja_log'
    out=[]
    if not os.path.exists(f): return out
    for l in open(f):
        p=l.split('\t')
        if len(p)<5: continue
        st,en,mt=int(p[0]),int(p[1]),int(p[2])
        if mt==0: continue
        endt=mt/1e9; out.append((endt-(en-st)/1e3,endt,os.path.basename(wt)+':'+os.path.basename(p[3])))
    return out
builds=[]
for wt in ['repo','wt-base','wt-mb-bloom','wt-mb-life']:
    b=ninja(f'{R}/{wt}')
    # merge into sessions (gap > 60 s)
    b.sort()
    cur=None
    for s,e,n in b:
        if e-s<2: continue  # only compile steps >= 2 s
        if cur and s-cur[1]<60: cur[1]=max(cur[1],e); cur[2]+=1
        else:
            if cur: builds.append(tuple(cur))
            cur=[s,e,1,wt]
    if cur: builds.append(tuple(cur))
f=lambda t: dt.datetime.fromtimestamp(t).strftime('%H:%M:%S')
iv.sort()
print('runs:',len(iv),'first',f(iv[0][0]),'last',f(iv[-1][1]))
print('GPU memory.used > 600 MiB at run start:',len(busy))
for b in busy[:20]: print('  ',b)
ov=0
for i in range(len(iv)-1):
    for j in range(i+1,len(iv)):
        if iv[j][0]>=iv[i][1]: break
        ov+=1; print('OVERLAP run/run',iv[i][2],f(iv[i][0]),f(iv[i][1]),'|',iv[j][2],f(iv[j][0]),f(iv[j][1]))
print('run/run overlaps:',ov)
print('build sessions (compile steps >= 2 s):')
for s,e,n,wt in sorted(builds):
    hit=[x for x in iv if x[0]<e and x[1]>s]
    print(f'  {wt:12s} {f(s)}-{f(e)} steps={n} overlapping GPU runs={len(hit)}')
    for x in hit[:40]: print('      ',x[2],f(x[0]),f(x[1]))
