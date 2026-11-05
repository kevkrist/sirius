import re,sys,collections
def agg(f):
    A=collections.Counter(); q=None; cur=None
    for l in open(f):
        if l.startswith('#Q'): q=int(l.split()[1]); continue
        m=re.match(r'== (.*?): n=(\d+) wall=([\d.]+)ms launched_kernels=([\d.]+)ms',l)
        if m:
            cur=m.group(1) if m.group(1) in ('sirius::compression::host_to_gpu','convert:host_to_gpu') else None
            if cur: A[(cur,'wall')]+=float(m.group(3)); A[(cur,'n')]+=int(m.group(2)); A[(cur,'kern')]+=float(m.group(4))
            continue
        if cur and l.strip().startswith('api:'):
            for k,v in re.findall(r'(\w+)=([\d.]+)',l): A[(cur,k)]+=float(v)
    return A
g1=agg('g1_prep_api.txt'); g4=agg('g4_prep_api.txt')
keys=sorted(set(g1)|set(g4), key=lambda k:(k[0],-(g4.get(k,0))))
for k in keys:
    if g4.get(k,0)>50 or g1.get(k,0)>50: print(f"{k[0][:34]:34s} {k[1]:28s} g1 {g1.get(k,0)/1000:7.2f}s  g4 {g4.get(k,0)/1000:7.2f}s  d {(g4.get(k,0)-g1.get(k,0))/1000:+6.2f}")
