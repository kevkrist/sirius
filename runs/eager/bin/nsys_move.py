#!/usr/bin/env python3
"""nsys_move.py <sqlite> [window=-1] [pipeline-substr=CONCAT] : who gets the copy path while the build CONCAT moves the build (read-only).
1. pipelines of the query window (NVTX 'Pipeline N Task T [..]'): n tasks, first start, last end (ms from window start).
2. the build-move pipeline = the CONCAT pipeline with the largest task time; per task: copies issued by its thread
   (runtime call -> memcpy activity by correlationId): GB per kind, copy time (sum of activity durations), queue wait
   (activity start - API call start: median / p90 / max), time in cudaStreamSynchronize.
3. per device during the move window: HtoD / DtoH / PtoP GB and busy fraction (union of activity intervals), split into
   'move' (issued by the CONCAT tasks' threads in their ranges) and 'other' (the scan)."""
import sqlite3, sys, collections, statistics as st
path = sys.argv[1]; wi = int(sys.argv[2]) if len(sys.argv) > 2 else -1; sub = sys.argv[3] if len(sys.argv) > 3 else 'CONCAT'
c = sqlite3.connect(f'file:{path}?mode=ro&immutable=1', uri=True).cursor()
S = dict(c.execute('select id,value from StringIds'))
nv = [(s, e, (S.get(tid) if t is None else t) or '', g) for s, e, t, tid, g in c.execute('select start,end,text,textId,globalTid from NVTX_EVENTS where end is not null')]
qw = sorted((s, e) for s, e, n, g in nv if n == 'sirius::query')
s0, e0 = qw[wi]; ms = lambda t: (t - s0) / 1e6
print('query windows ms:', [round((e - s) / 1e6, 1) for s, e in qw], 'using', wi)
tasks = collections.defaultdict(list)
for s, e, n, g in nv:
    if n.startswith('Pipeline ') and ' Task ' in n and s0 <= s <= e0:
        tasks[n.split(' Task ')[0] + ' ' + n[n.index('['):]].append((s, e, g))
print('\n| pipeline | tasks | first start | last end | sum task ms |')
for k, v in sorted(tasks.items(), key=lambda kv: min(x[0] for x in kv[1])):
    print(f"| {k[:70]} | {len(v)} | {ms(min(x[0] for x in v)):.1f} | {ms(max(x[1] for x in v)):.1f} | {sum(x[1]-x[0] for x in v)/1e6:.0f} |")
cand = {k: v for k, v in tasks.items() if k.split('[')[1].startswith(sub)}
key = max(cand, key=lambda k: sum(x[1] - x[0] for x in cand[k])); mv = cand[key]
print('\nbuild-move pipeline:', key)
KIND = {1: 'HtoD', 2: 'DtoH', 8: 'DtoD', 10: 'PtoP'}
def lab(kind, sk, dk):
    if sk == 2 and dk == 2: return 'P2P' if kind == 10 else 'D2D'
    if dk == 2: return 'H2D'
    if sk == 2: return 'D2H'
    return 'other'
mem = {r[0]: (r[1], r[2], r[3], r[4], lab(r[5], r[6], r[7])) for r in c.execute('select correlationId,start,end,deviceId,bytes,copyKind,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=?', (s0, e0))}
alloc = [(s, e, g) for s, e, n, g in nv if n == 'allocate_multiple_blocks' and s0 <= s <= e0]
rt = list(c.execute('select start,end,globalTid,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?', (s0, e0)))
move_corr = set()
print('\n| task | thread | start | end | dur | GB D2H | GB H2D | GB P2P | GB D2D | copies | copy time ms | queue wait med / p90 / max ms | in sync ms | sync calls | host-pool alloc ms |')
for a, b, g in sorted(mv):
    gb = collections.Counter(); ct = 0.0; waits = []; syn = 0.0; nsyn = 0; n = 0
    for rs, re_, tid, corr, nid in rt:
        if tid != g or rs < a or rs > b: continue
        name = S.get(nid, '')
        if 'Synchronize' in name: syn += (re_ - rs) / 1e6; nsyn += 1
        m = mem.get(corr)
        if m:
            n += 1; gb[m[4]] += m[3] / 1e9; ct += (m[1] - m[0]) / 1e6; waits.append((m[0] - rs) / 1e6); move_corr.add(corr)
    waits.sort(); q = lambda p: waits[min(len(waits) - 1, int(p * len(waits)))] if waits else 0
    print(f"| {ms(a):.0f} | {g % 100000} | {ms(a):.1f} | {ms(b):.1f} | {(b-a)/1e6:.1f} | {gb['D2H']:.2f} | {gb['H2D']:.2f} | {gb['P2P']:.2f} | {gb['D2D']:.2f} | {n} | {ct:.1f} | {q(.5):.1f} / {q(.9):.1f} / {q(1):.1f} | {syn:.1f} | {nsyn} | {sum(e-s_ for s_, e, g2 in alloc if g2 == g and a <= s_ <= b)/1e6:.1f} |")
A = min(x[0] for x in mv); B = max(x[1] for x in mv)
def union(iv):
    iv.sort(); tot = 0; cs = ce = None
    for s, e in iv:
        if cs is None: cs, ce = s, e
        elif s <= ce: ce = max(ce, e)
        else: tot += ce - cs; cs, ce = s, e
    return tot + (ce - cs if cs is not None else 0)
print(f'\nmove window {ms(A):.1f} .. {ms(B):.1f} ms ({(B-A)/1e6:.1f} ms). Per device: GB and busy fraction of the window')
print('| dev | kind | move GB | move busy % | other GB | other busy % | all busy % |')
per = collections.defaultdict(lambda: collections.defaultdict(list))
for corr, (s, e, dev, by, kind) in mem.items():
    if e < A or s > B: continue
    per[(dev, kind)]['move' if corr in move_corr else 'other'].append((max(s, A), min(e, B), by))
for (dev, kind), d in sorted(per.items()):
    w = B - A; f = lambda L: 100 * union([(s, e) for s, e, _ in L]) / w if L else 0
    print(f"| {dev} | {kind} | {sum(x[2] for x in d['move'])/1e9:.2f} | {f(d['move']):.0f} | {sum(x[2] for x in d['other'])/1e9:.2f} | {f(d['other']):.0f} | {f(d['move'] + d['other']):.0f} |")
