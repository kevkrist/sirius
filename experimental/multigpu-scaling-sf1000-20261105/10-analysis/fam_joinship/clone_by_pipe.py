#!/usr/bin/env python3
"""clone_by_pipe.py <sqlite> <start,end>: attribute convert:gpu_to_gpu:{host_staging,peer_copy} NVTX ranges to the enclosing
'Pipeline K Task N [..]' range (same thread). Per pipeline: tasks, task wall ms, #clones by kind, clone wall ms, bytes moved
(memcpys launched inside clone ranges), cuStreamSynchronize ms inside, clone per-call mean ms and effective GB/s per clone."""
import sqlite3, sys, bisect, re
from collections import defaultdict
c = sqlite3.connect(sys.argv[1]); w0, w1 = map(int, sys.argv[2].split(","))
S = dict(c.execute("select id,value from StringIds"))
tasks = defaultdict(list); clones = defaultdict(list)
for s, e, t, tid, g in c.execute("select start,end,text,textId,globalTid from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
    n = t if t is not None else S.get(tid, "")
    m = re.match(r"Pipeline (\d+) Task \d+ \[(.*)\]", n)
    if m: tasks[g].append((s, e, int(m.group(1)), m.group(2)))
    elif n in ("convert:gpu_to_gpu:host_staging", "convert:gpu_to_gpu:peer_copy"): clones[g].append((s, e, n.split(":")[-1]))
rt = defaultdict(list)
for s, e, g, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (w0, w1)):
    if g in clones: rt[g].append((s, e, S.get(nid, ""), corr))
for g in rt: rt[g].sort()
mem = {corr: (b, en - st) for corr, b, st, en in c.execute("select correlationId,bytes,start,end from CUPTI_ACTIVITY_KIND_MEMCPY where start>=?", (w0,))}
P = defaultdict(lambda: defaultdict(float)); lab = {}
for g, T in tasks.items():
    for s, e, p, l in T:
        P[p]["tasks"] += 1; P[p]["task_ms"] += (e - s) / 1e6; lab[p] = l
    T.sort(); starts = [t[0] for t in T]
    for s, e, kind in clones.get(g, []):
        i = bisect.bisect_right(starts, s) - 1
        p = T[i][2] if i >= 0 and T[i][1] >= e else -1
        a = P[p]; a["n_" + kind] += 1; a["ms_" + kind] += (e - s) / 1e6
        L = rt[g]; j = bisect.bisect_left(L, (s,))
        while j < len(L) and L[j][0] < e:
            st, en, nm, corr = L[j]
            if corr in mem: a["B_" + kind] += mem[corr][0]; a["cms_" + kind] += mem[corr][1] / 1e6
            if "StreamSynchronize" in nm: a["sync_" + kind] += (en - st) / 1e6
            j += 1
print(f"{'pipe':>4} {'label':34} {'tasks':>5} {'task_ms':>8} | {'nStg':>5} {'msStg':>7} {'GBStg':>6} {'copyms':>7} {'syncms':>7} | {'nP2P':>5} {'msP2P':>7} {'GBP2P':>6}")
for p in sorted(P):
    a = P[p]
    if a["n_host_staging"] + a["n_peer_copy"] == 0 and p >= 0: continue
    print(f"{p:4d} {lab.get(p,'(outside task)')[:34]:34} {a['tasks']:5.0f} {a['task_ms']:8.1f} | {a['n_host_staging']:5.0f} {a['ms_host_staging']:7.1f} {a['B_host_staging']/2e9:6.2f} {a['cms_host_staging']:7.1f} {a['sync_host_staging']:7.1f} | {a['n_peer_copy']:5.0f} {a['ms_peer_copy']:7.1f} {a['B_peer_copy']/1e9:6.2f}")
