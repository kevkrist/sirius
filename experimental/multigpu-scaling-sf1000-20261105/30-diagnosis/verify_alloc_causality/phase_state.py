"""Per executor-thread state inside a phase window: PARTITION task coverage, allocator calls, other CUDA API, OSRT waits.
usage: phase_state.py <sqlite> <range_start_ns> <range_end_ns> <nvtx task-range substring>
Phase window = [min start, max end] of NVTX ranges matching the substring inside the query window."""
import sqlite3, sys, collections
db, qs, qe, sub = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
c = sqlite3.connect(db).cursor()
rows = c.execute("""select n.start,n.end,n.globalTid from NVTX_EVENTS n left join StringIds s on n.textId=s.id
 where n.eventType=59 and n.start>=? and n.end<=? and coalesce(n.text,s.value) like ?""", (qs, qe, '%'+sub+'%')).fetchall()
ps, pe = min(r[0] for r in rows), max(r[1] for r in rows)
W = pe - ps
tids = sorted(set(r[2] for r in rows))
# task coverage per thread
cov = collections.Counter()
for s, e, t in rows: cov[t] += e - s
names = dict(c.execute("select id,value from StringIds").fetchall())
api = collections.defaultdict(lambda: collections.Counter()); cnt = collections.Counter()
for s, e, t, nid in c.execute("""select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where end>? and start<?""", (ps, pe)):
    if t not in cov: continue
    nm = names.get(nid, str(nid)).split('_v')[0]
    d = min(e, pe) - max(s, ps)
    k = 'alloc' if ('MallocFromPool' in nm or 'FreeAsync' in nm or 'MallocAsync' in nm) else nm
    api[t][k] += d; cnt[k] += 1
tot_alloc = sum(api[t]['alloc'] for t in tids)
tot_cov = sum(cov.values())
other = collections.Counter()
for t in tids:
    for k, v in api[t].items():
        if k != 'alloc': other[k] += v
# kernels per device in window
kern = c.execute("select deviceId,sum(min(end,?)-max(start,?)) from CUPTI_ACTIVITY_KIND_KERNEL where end>? and start<? group by deviceId", (pe, ps, ps, pe)).fetchall()
# concurrency of tasks: mean in flight
print(f"phase window {ps/1e6:.1f}-{pe/1e6:.1f} ms = {W/1e6:.1f} ms; tasks {len(rows)} on {len(tids)} threads")
print(f"task coverage thread-ms {tot_cov/1e6:.1f} -> mean tasks in flight {tot_cov/W:.2f} of {len(tids)} threads")
print(f"allocator calls in window (task threads): {cnt['alloc']}, thread-ms {tot_alloc/1e6:.1f} ({100*tot_alloc/tot_cov:.0f}% of task time), mean in allocator {tot_alloc/W:.2f}")
print("top other APIs thread-ms:", [(k, round(v/1e6,1), cnt[k]) for k, v in other.most_common(6)])
print("kernel busy ms per device:", [(d, round(v/1e6,1)) for d, v in kern])
