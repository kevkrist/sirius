#!/usr/bin/env python3
"""iterpipes.py <telemetry label dir> <q> <iter> [<iter> ...]: per pipeline (label), first task created, first Preparing, last end, n tasks,
median Preparing phase (c0 - pr) and median task duration, ms from Executing. One column block per iteration."""
import sys, glob, statistics as st
sys.path.insert(0, '/opt/dlami/nvme/dfperf/runs/eager/bin'); sys.argv_saved = sys.argv; 
import importlib.util
spec = importlib.util.spec_from_file_location('es', '/opt/dlami/nvme/dfperf/runs/eager/bin/esites.py')
src = open('/opt/dlami/nvme/dfperf/runs/eager/bin/esites.py').read().replace('\nmain()\n', '\n')
es = {}; exec(compile(src, 'esites', 'exec'), es)
d, q = sys.argv[1], sys.argv[2]; its = [int(x) for x in sys.argv[3:]]
sess = sorted(glob.glob(f'{d}/q{q}/*'))[0]
s = es['qload'].load(sess)
for it in its:
    P, wall = es['pipes'](s, it)
    print(f'--- iter {it} wall {wall:.1f} ms')
    for p in sorted(P.values(), key=lambda p: p['cr']):
        ds = p['ds']
        prep = [x.get('c0', x['end']) - x.get('pr', x['cr']) for x in ds]
        dur = [x['end'] - x.get('pr', x['cr']) for x in ds]
        print(f"{p['lab'][:46]:46s} n={len(ds):3d} cr={p['cr']:7.1f} first={min(x.get('pr', x['cr']) for x in ds):7.1f} end={p['end']:7.1f} prep_med={st.median(prep):6.1f} prep_max={max(prep):6.1f} dur_med={st.median(dur):6.1f}")
