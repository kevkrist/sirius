#!/usr/bin/env python3
"""vnsys.py <nsys.sqlite>: independent DF finish-chain accounting per hot query window (verifier's own; not nsys_df.py).
Finish = NVTX range dynfilter::accum::finish. A CUDA API call belongs to the finish if issued on the finish thread inside the range,
or on any other thread inside a dynfilter::accum::host_or range (none issue CUDA calls). Memcpys follow by correlationId."""
import sqlite3, sys, collections, bisect
KIND = {1: 'HtoD', 2: 'DtoH', 8: 'DtoD', 10: 'PtoP'}
def union(iv):
    iv = sorted(iv); tot = 0; cs = ce = None
    for a, b in iv:
        if ce is None or a > ce:
            if ce is not None: tot += ce - cs
            cs, ce = a, b
        else: ce = max(ce, b)
    return tot + (ce - cs if ce is not None else 0)
def overlap(a, b, iv):
    return sum(max(0, min(b, e) - max(a, s)) for s, e in iv)
def main(path):
    c = sqlite3.connect(path).cursor()
    S = dict(c.execute('select id,value from StringIds'))
    nv = [(S.get(tid) if t is None else t, s, e, g) for t, tid, s, e, g in c.execute('select text,textId,start,end,globalTid from NVTX_EVENTS') if e is not None]
    qw = sorted((s, e) for n, s, e, g in nv if n == 'sirius::query')
    rt = {}
    for s, e, g, corr, nid in c.execute('select start,end,globalTid,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME'):
        rt[corr] = (s, e, g, S.get(nid, '?').split('_v')[0])
    mem = list(c.execute('select start,end,deviceId,bytes,copyKind,srcKind,dstKind,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY'))
    for wi, (ws, we) in enumerate(qw):
        if wi == 0: continue
        fin = [(s, e, g) for n, s, e, g in nv if n == 'dynfilter::accum::finish' and s >= ws and e <= we]
        fiv = [(s, e) for s, e, g in fin]
        def in_fin(corr):
            r = rt.get(corr)
            if not r: return False
            return any(r[2] == g and r[0] >= s and r[0] <= e for s, e, g in fin)
        api = collections.Counter(); api_ms = collections.Counter()
        for corr, (s, e, g, name) in rt.items():
            if any(g == fg and s >= fs and s <= fe for fs, fe, fg in fin):
                api[name] += 1; api_ms[name] += (e - s) / 1e6
        fm = collections.defaultdict(lambda: [0, 0, 0.0]); om = collections.defaultdict(lambda: [0, 0, 0.0])
        odur_in = [0, 0, 0.0]; odur_out = [0, 0, 0.0]; fdev = collections.defaultdict(list)
        for s, e, d, b, k, sk, dk, corr in mem:
            if s < ws or e > we: continue
            # direction from memory kinds (1 pageable, 2 pinned, 3 device): robust where copyKind says PtoP
            # direction from memory kinds (0 pageable, 1 pinned, 2 device): robust where CUPTI's copyKind says PtoP for batch copies
            kind = 'D2H' if (sk == 2 and dk != 2) else 'H2D' if (dk == 2 and sk != 2) else ('P2P' if k == 10 else 'D2D')
            key = kind
            if in_fin(corr):
                a = fm[key]; a[0] += 1; a[1] += b; a[2] += (e - s) / 1e6; fdev[d].append((s, e, b, kind))
            else:
                a = om[key]; a[0] += 1; a[1] += b; a[2] += (e - s) / 1e6
                ov = overlap(s, e, fiv)
                t = odur_in if ov > 0.5 * (e - s) else odur_out
                t[0] += 1; t[1] += b; t[2] += (e - s) / 1e6
        print(f"== iter {wi}: query wall {(we-ws)/1e6:.1f} ms; finish ranges {len(fin)}: " + ", ".join(f"{(e-s)/1e6:.1f}" for s, e, g in fin) + f"; sum {sum(e-s for s,e,g in fin)/1e6:.1f} ms, union {union(fiv)/1e6:.1f} ms")
        sync = {k: (v, round(api_ms[k], 1)) for k, v in api.items() if 'Synchronize' in k}
        print("   finish-thread waits:", sync, "| total API calls", sum(api.values()), "| top API ms:", [(k, round(v, 1)) for k, v in api_ms.most_common(4)])
        tb = sum(v[1] for v in fm.values())
        print("   finish copies:", {k: (v[0], round(v[1] / 2**30, 3), round(v[2], 1)) for k, v in sorted(fm.items())}, f"total {tb/2**30:.2f} GiB")
        for d in sorted(fdev):
            v = fdev[d]; up = sum(b for s, e, b, k in v if k == 'H2D'); dn = sum(b for s, e, b, k in v if k == 'D2H')
            busy = union([(s, e) for s, e, b, k in v]); span = max(e for s, e, b, k in v) - min(s for s, e, b, k in v)
            print(f"     gpu{d}: D2H {dn/2**30:.2f} GiB, H2D {up/2**30:.2f} GiB, copy busy {busy/1e6:.1f} ms of span {span/1e6:.1f} ms -> {(up+dn)/2**30/(busy/1e9):.1f} GiB/s while busy")
        print("   other copies in query:", {k: (v[0], round(v[1] / 2**30, 2), round(v[2], 1)) for k, v in sorted(om.items())})
        r_in = odur_in[1] / 2**30 / (odur_in[2] / 1e3) if odur_in[2] else 0; r_out = odur_out[1] / 2**30 / (odur_out[2] / 1e3) if odur_out[2] else 0
        print(f"   other copies overlapping a finish: n={odur_in[0]} {odur_in[1]/2**30:.2f} GiB {odur_in[2]:.1f} ms ({r_in:.1f} GiB/s per copy-time); outside: n={odur_out[0]} {odur_out[1]/2**30:.2f} GiB {odur_out[2]:.1f} ms ({r_out:.1f} GiB/s)")
        for name in ('convert:gpu_to_gpu:host_staging', 'convert:gpu_to_gpu', 'sirius_physical_concat::execute', 'sirius::compression::host_to_gpu', 'dynfilter::accum::host_or', 'dynfilter::accum::late_wait', 'hash_partition'):
            v = [(s, e) for n, s, e, g in nv if n == name and s >= ws and e <= we]
            if v: print(f"   range {name}: n={len(v)} sum {sum(e-s for s,e in v)/1e6:.1f} ms, max {max(e-s for s,e in v)/1e6:.1f}, overlapping finish: {sum(1 for s,e in v if overlap(s,e,fiv)>0)} ranges / {sum(e-s for s,e in v if overlap(s,e,fiv)>0)/1e6:.1f} ms")
main(sys.argv[1])
