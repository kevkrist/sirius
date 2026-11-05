"""For each (query, pipeline id) at g4: nsys profiled phase window, allocator thread-time on task threads, counterfactual
phase if each alloc/free cost its g1 suite mean (8.0/2.9 us), and the un-profiled Quent phase span (g4_a, iters 2-4 mean).
usage: phase_cf.py q:pipe [q:pipe ...]"""
import sqlite3, sys, csv, collections, statistics
B = '/home/ubuntu/sirius-scaling-20261105/'
mp = {int(r['query']): r for r in csv.DictReader(open(B + '01-nsys/nsys2_g4/nsys/map.csv')) if r['iter'] == '2'}
tasks = list(csv.DictReader(open(B + '00-baseline/g4_a/quent/tasks.csv')))
for arg in sys.argv[1:]:
    q, p = map(int, arg.split(':'))
    r = mp[q]; c = sqlite3.connect(r['sqlite']).cursor(); qs, qe = int(r['range_start_ns']), int(r['range_end_ns'])
    rows = c.execute("""select n.start,n.end,n.globalTid from NVTX_EVENTS n left join StringIds s on n.textId=s.id
     where n.eventType=59 and n.start>=? and n.end<=? and coalesce(n.text,s.value) like ?""", (qs, qe, f'Pipeline {p} Task %')).fetchall()
    ps, pe = min(x[0] for x in rows), max(x[1] for x in rows); W = pe - ps
    cov = collections.Counter()
    for s, e, t in rows: cov[t] += e - s
    names = dict(c.execute("select id,value from StringIds").fetchall())
    at = 0; ref = 0; n = 0
    for s, e, t, nid in c.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where end>? and start<?", (ps, pe)):
        if t not in cov: continue
        nm = names.get(nid, '')
        if 'MallocFromPool' in nm: at += e - s; ref += 8000; n += 1
        elif 'FreeAsync' in nm: at += e - s; ref += 2900; n += 1
    inflight = sum(cov.values()) / W
    cf = W - (at - ref) / inflight
    # unprofiled phase span from Quent (pipeline type 'Pipeline Id p')
    spans = []
    for it in (2, 3, 4):
        ts = [t for t in tasks if t['query'] == f'tpch_q{q}_iter{it}' and t['pipeline_type'] == f'Pipeline Id {p}']
        if ts: spans.append((max(int(t['finalize']) for t in ts) - min(int(t['preparing']) for t in ts)) / 1e6)
    un = statistics.mean(spans) if spans else float('nan')
    print(f"Q{q} P{p}: tasks {len(rows)} nsys phase {W/1e6:.0f} ms, inflight {inflight:.1f}, alloc calls {n}, alloc thread-ms {at/1e6:.0f} "
          f"(excess {(at-ref)/1e6:.0f}) -> cf {cf/1e6:.0f} ms, save_prof {(W-cf)/1e6:.0f}; unprof phase {un:.0f} ms (ratio {un/(W/1e6):.2f}) "
          f"-> save_unprof~{(W-cf)/1e6*un/(W/1e6):.0f}")
