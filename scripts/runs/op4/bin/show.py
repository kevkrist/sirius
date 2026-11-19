import sys,collections
rows=[]
for l in open(sys.argv[1]):
    if not l.startswith('RES,setprobe'): continue
    d=dict(kv.split('=',1) for kv in l.strip().split(',')[2:]); rows.append(d)
groups=collections.OrderedDict()
for d in rows:
    k=(d['label'],d['key'],d['probe_key'],d['members'],d['factor'],d['bpk'],d['hit_permille'],d['prior_permille'],d['run'],d['asc'])
    groups.setdefault(k,[]).append(d)
for k,v in groups.items():
    d=v[0]
    print(f"{k[0]:8s} key{k[1]}/probe{k[2]} members={int(k[3])/1e3:.0f}K factor={k[4]} bpk={k[5]} hit={k[6]} prior={k[7]} run={k[8]} asc={k[9]} set={int(d['set_bytes'])/2**20:.1f}MiB pow2={int(d['pow2_bytes'])/2**20:.1f}MiB bloom={int(d['bloom_bytes'])/2**20:.2f}MiB")
    print("     "+"  ".join(f"{x['variant']}={float(x['Grows_s']):.0f}" for x in v)+"   [G rows/s]")
    fp=[f"{x['variant']} fpr={float(x['fpr_live']):.5f}" for x in v if x['variant'] in('bloom','bloomprod')]
    sp=[f"{x['variant']} spread={(float(x['ms_max'])/float(x['ms_min'])-1)*100:.0f}%" for x in v if float(x['ms_max'])/float(x['ms_min'])>1.05]
    print("     "+"; ".join(fp+sp))
