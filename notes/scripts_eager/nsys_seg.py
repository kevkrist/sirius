#!/usr/bin/env python3
"""nsys_seg.py <sqlite> [window=-1] [task-index=0]: one build-move CONCAT task, per synced segment (one buffer):
submit time, MiB, D2H first start..last end, H2D/P2P first start..last end, sync return; ms from task start."""
import sqlite3, sys, collections
path = sys.argv[1]; wi = int(sys.argv[2]) if len(sys.argv) > 2 else -1; ti = int(sys.argv[3]) if len(sys.argv) > 3 else 0
c = sqlite3.connect(f'file:{path}?mode=ro&immutable=1', uri=True).cursor()
S = dict(c.execute('select id,value from StringIds'))
nv = [(s, e, (S.get(tid) if t is None else t) or '', g) for s, e, t, tid, g in c.execute('select start,end,text,textId,globalTid from NVTX_EVENTS where end is not null')]
qw = sorted((s, e) for s, e, n, g in nv if n == 'sirius::query'); s0, e0 = qw[wi]
tasks = collections.defaultdict(list)
for s, e, n, g in nv:
    if n.startswith('Pipeline ') and ' Task ' in n and s0 <= s <= e0 and n[n.index('[') + 1:].startswith('CONCAT'): tasks[n.split(' Task ')[0]].append((s, e, g))
key = max(tasks, key=lambda k: sum(x[1] - x[0] for x in tasks[k])); a, b, g = sorted(tasks[key])[ti]
def lab(kind, sk, dk):
    if sk == 2 and dk == 2: return 'P2P' if kind == 10 else 'D2D'
    return 'H2D' if dk == 2 else 'D2H'
mem = {r[0]: (r[1], r[2], r[3], r[4], lab(r[5], r[6], r[7])) for r in c.execute('select correlationId,start,end,deviceId,bytes,copyKind,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=?', (s0, e0))}
rt = sorted(r for r in c.execute('select start,end,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where globalTid=? and start>=? and start<=?', (g, a, b)))
ms = lambda t: (t - a) / 1e6
print(f'task {key} thread {g % 100000} dur {(b-a)/1e6:.1f} ms')
print('| seg | submit | MiB | D2H dev: start..end | H2D/P2P dev: start..end | D2D | sync returns | seg wall |')
seg = []; n = 0; t_sub = None
for rs, re_, corr, nid in rt:
    name = S.get(nid, ''); m = mem.get(corr)
    if m:
        if t_sub is None: t_sub = rs
        seg.append(m)
    if 'StreamSynchronize' in name and seg:
        by = collections.defaultdict(list)
        for x in seg: by[x[4]].append(x)
        f = lambda k: (f"{by[k][0][2]}: {ms(min(x[0] for x in by[k])):.1f}..{ms(max(x[1] for x in by[k])):.1f}" if by[k] else '-')
        hk = 'H2D' if by['H2D'] else 'P2P'
        print(f"| {n} | {ms(t_sub):.1f} | {sum(x[3] for x in seg if x[4] in ('H2D', 'P2P', 'D2D'))/1048576:.0f} | {f('D2H')} | {hk} {f(hk)} | {f('D2D')} | {ms(re_):.1f} | {(re_-t_sub)/1e6:.1f} |")
        seg = []; t_sub = None; n += 1
