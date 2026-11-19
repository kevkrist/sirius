#!/usr/bin/env python3
"""nsys_xport.py <nsys.sqlite>: per hot iteration, bytes by transport inside dynfilter::accum::finish
(copies issued by the finish thread, followed by correlation id)."""
import sqlite3, sys, bisect, collections
c = sqlite3.connect(sys.argv[1]).cursor()
S = dict(c.execute('select id,value from StringIds'))
cols = [r[1] for r in c.execute('pragma table_info(CUPTI_ACTIVITY_KIND_MEMCPY)')]
nv = [(S.get(tid) if t is None else t, s, e, g) for t, tid, s, e, g in c.execute('select text,textId,start,end,globalTid from NVTX_EVENTS') if e is not None]
qw = sorted((s, e) for n, s, e, g in nv if n == 'sirius::query')
fin = [(s, e, g) for n, s, e, g in nv if n == 'dynfilter::accum::finish']
rt = collections.defaultdict(list)
for s, e, g, corr, nid in c.execute('select start,end,globalTid,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME order by start'):
    rt[g].append((s, e, corr, S.get(nid, '?').split('_v')[0]))
M = collections.defaultdict(list)
for s, e, b, k, sk, dk, sd, dd, corr in c.execute('select start,end,bytes,copyKind,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY'):
    M[corr].append((s, e, b, k, sk, dk, sd, dd))
MiB = 2 ** 20
print("iter | finish ranges | wall ms (sum; each) | host syncs (StreamSynchronize) | D2H MiB | H2D MiB | P2P MiB (cudaMemcpyPeerAsync) | other MiB | copy ops | last copy end - range end ms")
for wi, (ws, we) in enumerate(qw):
    if wi == 0: continue
    rs = [r for r in fin if ws <= r[0] < we]
    tot = collections.Counter(); syncs = 0; ops = 0; walls = []; tail = []
    for s, e, g in rs:
        walls.append((e - s) / 1e6); last = s
        for cs, ce, corr, name in rt[g]:
            if cs < s or cs >= e: continue
            if 'StreamSynchronize' in name: syncs += 1
            for ms, me, b, k, sk, dk, sd, dd in M.get(corr, []):
                ops += 1; last = max(last, me)
                # CUPTI's copyKind / device ids are not usable for batch copies (MB_TRANSFER section 10): classify by API.
                # cudaMemcpyBatchAsync = one HOST-staged leg, direction from the memory kinds (1 = pinned host, 2 = device).
                if 'MemcpyPeer' in name: tot['peer'] += b
                elif 'MemcpyBatch' in name or name.endswith('MemcpyAsync'):
                    if (sk, dk) == (2, 1): tot['d2h'] += b
                    elif (sk, dk) == (1, 2): tot['h2d'] += b
                    else: tot['d2d'] += b
                else: tot['d2d'] += b
        tail.append((last - e) / 1e6)
    print(f"{wi} | {len(rs)} | {sum(walls):.1f}; {', '.join(f'{w:.1f}' for w in walls)} | {syncs} | {tot['d2h']/MiB:.0f} | {tot['h2d']/MiB:.0f} | {tot['peer']/MiB:.0f} | {tot['d2d']/MiB:.0f} | {ops} | {', '.join(f'{t:.2f}' for t in tail)}")
