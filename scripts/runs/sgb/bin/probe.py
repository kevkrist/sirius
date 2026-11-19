#!/usr/bin/env python3
# probe.py <log_dir or log file ...> : per window, counts of "join filters kept" lines by (N of M probes ran), kept/total rows; DF key lines
import sys,re,glob,collections,os
for a in sys.argv[1:]:
    fs=sorted(glob.glob(a+'/log_dir/sirius_*.log')) if os.path.isdir(a) else [a]
    win=0; C=collections.Counter(); K=[0,0]; keys=collections.Counter(); out=[]
    for f in fs:
        for l in open(f,errors='ignore'):
            if '[window] end' in l or 'window] end' in l:
                out.append((dict(C),K[:],dict(keys))); C=collections.Counter(); K=[0,0]; keys=collections.Counter()
            m=re.search(r'join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe',l)
            if m: C['%s of %s'%(m.group(3),m.group(4))]+=1; K[0]+=int(m.group(1)); K[1]+=int(m.group(2))
            if '[dynamic_filter_key]' in l:
                m=re.search(r'join=(\d+).*?rows=(\d+).*?kind=(\w+) size=(\d+).*?pushed=(\d+) reason=(\S+)',l)
                keys[m.groups() if m else l.split('[dynamic_filter_key]')[1].strip()[:80]]+=1
    print(a)
    for i,(c,k,ks) in enumerate(out): print('  window',i,'probes',c,'kept/rows',k,'keys',sorted(ks.items()))
