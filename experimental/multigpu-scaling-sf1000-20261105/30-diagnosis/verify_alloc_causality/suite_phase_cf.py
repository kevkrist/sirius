"""Independent suite price of the driver-allocator excess at g4, per pipeline phase, with BOTH a link floor and a
per-GPU kernel floor (X1 used only a link cap). nsys2_g4 iter2 windows.
Per pipeline K (NVTX 'Pipeline K Task ...'): W = phase span, n = mean tasks in flight, E = sum over allocator calls made
inside that pipeline's task ranges of (duration - g1 suite mean 8.0 us malloc / 2.9 us free).
cf = max(W - E/n, max-GPU kernel-busy in W, H2D bytes in W / 110 GB/s). save = W - cf.
Un-profiled: x Quent span ratio for that pipeline (g4_a iters 2-4) when pipeline ids match, else x0.75.
LINCAP=1 additionally caps each phase saving at its own gap to linear (g4 span - g1 span/4, Quent un-profiled).
Overlapping phases: savings are reported raw and also scaled by union(W)/sum(W) per query."""
import sqlite3, csv, collections, bisect, statistics, sys, os
LINCAP = os.environ.get('LINCAP') == '1'
B = '/home/ubuntu/sirius-scaling-20261105/'
mp = {int(r['query']): r for r in csv.DictReader(open(B + '01-nsys/nsys2_g4/nsys/map.csv')) if r['iter'] == '2'}
tasks = list(csv.DictReader(open(B + '00-baseline/g4_a/quent/tasks.csv')))
tasks1 = list(csv.DictReader(open(B + '00-baseline/g1_a/quent/tasks.csv')))
byqp1 = collections.defaultdict(list)
for t in tasks1:
    if t['preparing']: byqp1[(t['query'], t['pipeline_type'])].append(t)
qspan = collections.defaultdict(list)
byqp = collections.defaultdict(list)
for t in tasks:
    if t['preparing']: byqp[(t['query'], t['pipeline_type'])].append(t)
def unprof_span(q, p, d=None):
    d = byqp if d is None else d
    s = []
    for it in (2, 3, 4):
        ts = d.get((f'tpch_q{q}_iter{it}', f'Pipeline Id {p}'))
        if ts: s.append((max(int(t['finalize']) for t in ts) - min(int(t['preparing']) for t in ts)) / 1e6)
    return statistics.mean(s) if s else None
tot_raw = tot_un = tot_un_ov = 0
out = []
for q in sorted(mp):
    r = mp[q]; c = sqlite3.connect(r['sqlite']).cursor(); qs, qe = int(r['range_start_ns']), int(r['range_end_ns'])
    names = dict(c.execute("select id,value from StringIds").fetchall())
    nv = c.execute("""select n.start,n.end,n.globalTid,coalesce(n.text,s.value) from NVTX_EVENTS n left join StringIds s on n.textId=s.id
       where n.eventType=59 and n.start>=? and n.end<=? and coalesce(n.text,s.value) like 'Pipeline % Task %'""", (qs, qe)).fetchall()
    pipes = collections.defaultdict(list)
    for s, e, t, nm in nv: pipes[int(nm.split()[1])].append((s, e, t))
    # per-thread interval index -> pipeline
    per_thr = collections.defaultdict(list)
    for p, L in pipes.items():
        for s, e, t in L: per_thr[t].append((s, e, p))
    for t in per_thr: per_thr[t].sort()
    starts = {t: [x[0] for x in L] for t, L in per_thr.items()}
    E = collections.Counter()
    for s, e, t, nid in c.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (qs, qe)):
        nm = names.get(nid, '')
        if 'MallocFromPool' in nm: ref = 8000
        elif 'FreeAsync' in nm: ref = 2900
        else: continue
        L = per_thr.get(t)
        if not L: continue
        i = bisect.bisect_right(starts[t], s) - 1
        if i >= 0 and L[i][1] >= e: E[L[i][2]] += (e - s) - ref
    kern = c.execute("select deviceId,start,end from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?", (qs, qe)).fetchall()
    h2d = c.execute("select start,end,bytes from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=? and (copyKind=1 or (copyKind=10 and srcKind=1 and dstKind=2))", (qs, qe)).fetchall()
    qsave_raw = qsave_un = 0; sumW = 0; ivs = []
    for p, L in pipes.items():
        ps, pe = min(x[0] for x in L), max(x[1] for x in L); W = pe - ps
        if W <= 0: continue
        n = sum(x[1] - x[0] for x in L) / W
        kb = collections.Counter()
        for d, s, e in kern:
            if e > ps and s < pe: kb[d] += min(e, pe) - max(s, ps)
        kfloor = max(kb.values()) if kb else 0
        lb = sum(b * (min(e, pe) - max(s, ps)) / max(1, e - s) for s, e, b in h2d if e > ps and s < pe)
        lfloor = lb / 110e9 * 1e9
        cf = max(W - max(E[p], 0) / n, kfloor, lfloor)
        save = W - cf
        u = unprof_span(q, p); ratio = (u / (W / 1e6)) if u else 0.75
        ratio = min(max(ratio, 0.5), 1.0)
        su = save * ratio
        u1 = unprof_span(q, p, byqp1)
        if LINCAP and u and u1 is not None: su = max(0.0, min(su, (u - u1 / 4) * 1e6))
        elif os.environ.get("MATCHED_ONLY") == "1": su = 0.0
        qsave_raw += save; qsave_un += su; sumW += W; ivs.append((ps, pe))
    ivs.sort(); un = 0; cs, ce = None, None
    for s, e in ivs:
        if cs is None or s > ce:
            if cs is not None: un += ce - cs
            cs, ce = s, e
        else: ce = max(ce, e)
    if cs is not None: un += ce - cs
    ov = un / sumW if sumW else 1
    tot_raw += qsave_raw; tot_un += qsave_un; tot_un_ov += qsave_un * ov
    out.append((q, qsave_raw / 1e6, qsave_un / 1e6, qsave_un * ov / 1e6, ov))
    print(f"Q{q}: save profiled {qsave_raw/1e6:.0f} ms, un-profiled {qsave_un/1e6:.0f}, overlap-scaled {qsave_un*ov/1e6:.0f} (union/sum {ov:.2f})", flush=True)
print(f"SUITE: profiled {tot_raw/1e9:.2f} s, un-profiled {tot_un/1e9:.2f} s, overlap-scaled {tot_un_ov/1e9:.2f} s")
