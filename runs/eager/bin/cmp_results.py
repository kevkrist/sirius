#!/usr/bin/env python3
"""cmp_results.py <labelsA,comma> <labelsB,comma> [q ...]: byte-compare sirius/q<N>/result.txt across all runs of both label sets."""
import glob,hashlib,sys
B="/opt/dlami/nvme/dfperf/runs/eager"
A=sys.argv[1].split(","); Bl=sys.argv[2].split(","); qs=[int(x) for x in sys.argv[3:]] or range(1,23)
bad=0; n=0
for q in qs:
    h={}
    for l in A+Bl:
        for d in sorted(glob.glob(f"{B}/{l}/tpch_*_q{q}")):
            fs=glob.glob(f"{d}/sirius/q{q}/result*")
            for f in fs: h[(l,f.split('/')[-1])]=hashlib.md5(open(f,'rb').read()).hexdigest()
    vals=set(h.values()); n+=1
    if len(vals)!=1:
        bad+=1; print(f"q{q}: MISMATCH or missing: {len(vals)} distinct of {len(h)} files: "+", ".join(f"{k[0]}={v[:8]}" for k,v in h.items()))
    else: print(f"q{q}: identical ({len(h)} files, md5 {next(iter(vals))[:8]})")
print(f"{n-bad}/{n} queries identical")
