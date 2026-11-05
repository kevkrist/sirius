import re,sys; PP,CP=sys.argv[1],sys.argv[2]
# bandwidth floor for a shuffle at g4 on this topology: remote = 3/4 of bytes; 2/3 of remote crosses switches (host-staged,
# 77 GB/s aggregate for 4 cross pairs, microbench p2p) -> 8.7 ms per GB remote. Concat pipelines with GPU-prep < 5 ms moved nothing.
T=0;TF=0
for q in [5,7,8,9]:
    gb={}
    for l in open(f'{PP}{q}.txt'):
        m=re.match(r'(P\d+)\s+(CONCAT\(\d+\))\s+'+r'\s+'.join([r'(\d+)']*8)+r'\s+([\d.]+)\s+([\d.]+)',l)
        if m: gb[m.group(1)]=(float(m.group(12)),float(m.group(8)))
    tot_ex=tot_fl=0; rows=[]
    for l in open(f'{CP}{q}.txt'):
        m=re.match(r'(P\d+)\s+CONCAT.*?\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)$',l.rstrip())
        if not m: continue
        p=m.group(1); ex=float(m.group(5)); g,pg=gb[p]
        fl=0 if pg<5 else g*0.75*(2/3)/77*1000
        tot_ex+=ex; tot_fl+=min(fl,max(ex,0)); rows.append(f"{p}:{g:.1f}GB pG{pg:.0f} ex{ex:.0f} fl{fl:.0f}")
    T+=tot_ex;TF+=tot_fl
    print(f"Q{q} CLONE excess {tot_ex:.0f} bw-floor {tot_fl:.0f} overhead {tot_ex-tot_fl:.0f} | "+'; '.join(rows))
print(f"FAM excess {T:.0f} floor {TF:.0f} overhead {T-TF:.0f}")
