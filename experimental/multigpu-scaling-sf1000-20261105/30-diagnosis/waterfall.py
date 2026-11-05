#!/usr/bin/env python3
"""Root-cause waterfall: family decompositions (family_map.csv) -> buckets, plus the documented
synthesizer adjustments. Prints bucket x family matrix (ms, g4 wall) and the final waterfall."""
import csv, collections
fams = ['scanagg','joinship','semijoin','multijoin','groupby','small','subq']
M = collections.defaultdict(lambda: collections.defaultdict(float))
for r in csv.DictReader(open('family_map.csv')):
    M[r['bucket']][r['family']] += float(r['ms'])
order = ['LINK','ALLOC','FRAG','BYTES','POLICY','BLOOM','GRAN','FIXED','MEAS','RESID']
print('bucket    ' + ' '.join(f'{f:>9}' for f in fams) + '      sum')
tot = collections.defaultdict(float)
for b in order:
    s = sum(M[b].values()); tot[b] = s
    print(f'{b:9} ' + ' '.join(f'{M[b][f]:9.0f}' for f in fams) + f' {s:8.0f}')
print('family gap', [round(sum(M[b][f] for b in order)) for f in fams], round(sum(tot.values())))
# synthesizer adjustments (see RANKED.md section 'Waterfall construction')
HW_FLOOR = 3480            # X3 L1 per-query hard floor: sum max(0, scan bytes/105 GB/s - T1/4)
FREES_FROM_FRAG = 300      # X1: task-end frees 0.63 s profiled (~0.47 un-profiled) land at the end of consumer tasks
H1 = 200                   # X1: small-pinned cross-GPU pooled-event waits, 0.36 s profiled x 0.57-0.92, carved from per-op inflation
OUTSIDE_SUITE = 336        # outside_window.py: sum over 22 queries of (out_g4 - out_g1/4)
w = collections.OrderedDict()
w['LINK_HW (hard floor)'] = HW_FLOOR
w['LINK_SERIAL (link-bound phases not overlapped)'] = tot['LINK'] - HW_FLOOR
w['ALLOC (driver allocator lock)'] = tot['ALLOC'] + FREES_FROM_FRAG - H1
w['FRAG (per-fragment clone overhead)'] = tot['FRAG'] - FREES_FROM_FRAG
w['BYTES (cross-switch shuffle bytes, 2-leg staging)'] = tot['BYTES']
w['POLICY (exchanges that do not exist at N=1 / should be broadcast)'] = tot['POLICY']
w['FIXED (outside query window)'] = OUTSIDE_SUITE
w['GRAN (P mod N, quantization, tails)'] = tot['GRAN']
w['BLOOM (host-staged accumulated publication)'] = tot['BLOOM']
w['H1 (small-pinned cross-GPU event waits)'] = H1
w['MEAS (g1 artifacts credit)'] = tot['MEAS']
w['RESID (unexplained)'] = tot['RESID'] - (OUTSIDE_SUITE - tot['FIXED'])
cum = 8960
print(f'\nideal {cum}')
for k, v in w.items():
    cum += v; print(f'{k:62} {v:7.0f}  {100*v/11083:5.1f}%  cum {cum:7.0f}')
