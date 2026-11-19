#!/usr/bin/env python3
"""lp_cmp.py <tip label dir> <sgb label dir> q [q...] : trace-log pass diff per hot window (last window of each process).
Compares: [dynamic_filter_key] lines (join, rows, valid, kind, size, zone_map, pushed, reason), 'construction started' count,
'constructed filters published' count + flavor, Bloom finish lines (bytes), compressed-scan 'join filters kept' (N of M, kept, total),
per-operator produced rows summed over tasks (operator name,id), fallback/error lines. Prints DIFF lines; 'same' otherwise."""
import sys,re,glob,collections,os
def parse(label,q):
    fs=sorted(glob.glob('%s/tpch_*_q%s/log_dir/sirius_*.log'%(label,q)))
    if not fs: return None
    wins=[]; cur=None
    for f in fs:
        for l in open(f,errors='ignore'):
            if '[window] begin' in l:
                cur=dict(keys=collections.Counter(),started=0,published=collections.Counter(),finish=collections.Counter(),probes=collections.Counter(),
                         kept=[0,0],ops=collections.Counter(),optasks=collections.Counter(),bad=0,ok=None,skips=collections.Counter(),tasks_peak=collections.defaultdict(int))
                continue
            if cur is None: continue
            if '[window] end' in l:
                cur['ok']='outcome=ok' in l; wins.append(cur); cur=None; continue
            if '[dynamic_filter_key]' in l:
                m=re.search(r'join=(\d+) condition=(\d+) build_ordinal=(\d+) rows=(\d+) valid=(\d+) kind=(\S+) size=(\d+) zone_map=(\d+) pushed=(\d+) reason=(\S+)',l)
                cur['keys'][m.groups() if m else l.split('[dynamic_filter_key]')[1].strip()]+=1
            elif 'construction started' in l: cur['started']+=1
            elif 'constructed filters published' in l:
                m=re.search(r'published( \([^)]*\))?: (\d+) filter\(s\) pushed to (\d+) target',l)
                fl=(m.group(1) or '').strip(); fl='bloom-transport' if ('HOST' in fl or 'PROTO' in fl) else fl
                cur['published'][(fl,m.group(2),m.group(3))]+=1
                if 'PROTO' in l: cur['skips']['local_published']+=1
            elif 'accumulated_bloom_builder::finish]' in l:
                m=re.search(r'(\d+) key\(s\), (\d+) bytes in all',l); cur['finish'][m.groups() if m else '?']+=1
            elif 'join filters kept' in l:
                m=re.search(r'kept (\d+)/(\d+) rows \((\d+) of (\d+) probe',l)
                cur['probes']['%s of %s'%(m.group(3),m.group(4))]+=1; cur['kept'][0]+=int(m.group(1)); cur['kept'][1]+=int(m.group(2))
            elif ' produced ' in l and 'operator ' in l:
                m=re.search(r'operator (\S+) \(id=(\d+)\).*?num rows: ([\d ]+?),.*?peak allocated: (\d+)',l)
                if m:
                    k=(m.group(1),int(m.group(2))); cur['ops'][k]+=sum(int(x) for x in m.group(3).split()); cur['optasks'][k]+=1
                    cur['tasks_peak'][k]=max(cur['tasks_peak'][k],int(m.group(4)))
            elif re.search(r'execution fallback|fallback to DuckDB|falling back|\[error\]|out_of_memory|OOM retry',l): cur['bad']+=1
            elif 'construction declined' in l or 'attempt_skip' in l: cur['skips']['declined/skip']+=1
    return wins
A,B=sys.argv[1:3]
for q in sys.argv[3:]:
    a=parse(A,q); b=parse(B,q)
    if not a or not b: print('q%s: MISSING (%s, %s)'%(q,bool(a),bool(b))); continue
    wa,wb=a[-1],b[-1]; d=[]
    for key in ('keys','started','finish','probes','kept','ops','optasks','bad','ok'):
        if wa[key]!=wb[key]:
            if isinstance(wa[key],collections.Counter):
                ks=set(wa[key])|set(wb[key]); diff=[(k,wa[key].get(k),wb[key].get(k)) for k in sorted(ks,key=str) if wa[key].get(k)!=wb[key].get(k)]
                d.append('%s: %s'%(key,diff[:8]))
            else: d.append('%s: %s vs %s'%(key,wa[key],wb[key]))
    pa=sum(wa['published'].values()); pb=sum(wb['published'].values())
    if wa['published']!=wb['published']: d.append('published: %s vs %s'%(dict(wa['published']),dict(wb['published'])))
    nkeys=sum(wa['keys'].values()); nbloom=sum(v for k,v in wa['keys'].items() if isinstance(k,tuple) and 'bloom' in k[5])
    pk=lambda w,name: max([v for (n,i),v in w['tasks_peak'].items() if n==name] or [0])/1e6
    print('q%s: windows %d/%d ok=%s/%s | keys %d (bloom %d) started %d published %d/%d local_published(sgb) %d | scan probes %s kept %s | ops %d tasks %d/%d | task peak MB PARTITION %.0f/%.0f CONCAT %.0f/%.0f | %s'%(
        q,len(a),len(b),wa['ok'],wb['ok'],nkeys,nbloom,wa['started'],pa,pb,wb['skips']['local_published'],dict(wa['probes']),wa['kept'],len(wa['ops']),
        sum(wa['optasks'].values()),sum(wb['optasks'].values()),pk(wa,'PARTITION'),pk(wb,'PARTITION'),pk(wa,'CONCAT'),pk(wb,'CONCAT'),'SAME' if not d else 'DIFF'))
    for x in d: print('     DIFF',x)
