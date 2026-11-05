#!/usr/bin/env python3
"""x1_nsys.py <map.csv> <out.json> [queries...]

Per query (iter=2 window only), read-only on the nsys sqlite:
  api      : per CUDA API name: calls, total ns
  lock     : OSRT lock ns (rwlock_wr/rd, mutex, futex) nested inside each CUDA API name on the same thread
             ('<none>' = user-space locks outside any CUDA call)
  conc     : allocator (malloc+free) latency binned by number of allocator calls in flight at call start
  occ      : allocator union-occupancy ns (>=1 thread inside malloc/free), window ns
  attr     : malloc/free calls + ns per op-level NVTX range, and per leaf NVTX range
  tasks    : Task NVTX ranges per op signature: count, malloc count
  sync     : cudaStreamSynchronize / cuStreamSynchronize split into own-stream-busy / stream-blocked / tail / no-work,
             per op-level range
"""
import sqlite3, csv, sys, json, re, bisect
from collections import defaultdict, Counter

ALLOC = ('cudaMallocFromPoolAsync', 'cudaFreeAsync')
SYNCS = ('cudaStreamSynchronize', 'cuStreamSynchronize')
LOCKS = ('pthread_rwlock_wrlock', 'pthread_rwlock_rdlock', 'pthread_mutex_lock', 'futex')
num = re.compile(r'\d+')


def norm(n):
    return num.sub('#', n or '')


def oplevel(stack_names):
    """stack_names outer->inner (normalized). Return op-level label."""
    lab = None
    in_task = False
    for n in stack_names:
        if n.startswith('Pipeline # Task #'):
            in_task = True
        m = re.match(r'Pipeline #: ([A-Z_]+) \(id=#\)', n)
        if m:
            lab = 'op:' + m.group(1)
        elif n.startswith('convert:') and (lab is None or not lab.startswith('op:')):
            lab = 'prep:' + n.split(':')[1] if lab is None else lab
        elif n == 'sirius::compression::host_to_gpu' and lab is None:
            lab = 'prep:compressed_host_to_gpu'
    if lab is None:
        lab = 'task:other' if in_task else 'outside-task'
    return lab


def bucket(k):
    return '1' if k <= 1 else '2' if k == 2 else '3-4' if k <= 4 else '5-8' if k <= 8 else '9-12' if k <= 12 else '13+'


def analyze(db, w0, w1):
    c = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    S = dict(c.execute('select id,value from StringIds'))
    R = {}
    rt = []  # (start,end,tid,name,corr)
    for s, e, t, n, corr in c.execute(
            'select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?', (w0, w1)):
        rt.append((s, e, t, S[n].split('_v')[0], corr))
    api = defaultdict(lambda: [0, 0])
    for s, e, t, n, corr in rt:
        api[n][0] += 1; api[n][1] += e - s
    R['api'] = dict(api)
    R['window_ns'] = w1 - w0

    # per-thread runtime intervals (runtime calls don't nest except rare driver-inside-runtime; handle by containment list)
    by_t = defaultdict(list)
    for r in rt:
        by_t[r[2]].append(r)
    for t in by_t:
        by_t[t].sort()
    starts = {t: [r[0] for r in v] for t, v in by_t.items()}

    # OSRT locks nested
    lock = defaultdict(lambda: defaultdict(int))
    lockcnt = defaultdict(lambda: defaultdict(int))
    lid = {i for i, v in S.items() if v in LOCKS}
    for s, e, t, n, cc in c.execute('select start,end,globalTid,nameId,callchainId from OSRT_API where start>=? and end<=?', (w0, w1)):
        if n not in lid:
            continue
        host = '<none>'
        v = by_t.get(t)
        if v:
            i = bisect.bisect_right(starts[t], s) - 1
            # walk back a few to find an enclosing call (driver call nested in runtime call)
            j = i
            while j >= 0 and j >= i - 3:
                if v[j][0] <= s and v[j][1] >= e:
                    host = v[j][3]
                    # prefer outer runtime (cuda*) over inner driver (cu*) if both enclose
                    if host.startswith('cu') and not host.startswith('cuda') and j > 0 and v[j - 1][0] <= s and v[j - 1][1] >= e:
                        host = v[j - 1][3]
                    break
                j -= 1
        lock[host][S[n]] += e - s
        lockcnt[host][S[n]] += 1
    R['lock'] = {k: dict(v) for k, v in lock.items()}
    R['lockcnt'] = {k: dict(v) for k, v in lockcnt.items()}

    # allocator concurrency
    al = [(s, e) for s, e, t, n, corr in rt if n in ALLOC]
    ev = []
    for s, e in al:
        ev.append((s, 1)); ev.append((e, -1))
    ev.sort()
    # in-flight count at each call start: sweep
    al_sorted = sorted(al)
    conc = defaultdict(lambda: [0, 0])
    k = 0; ei = 0
    occ = 0; last = None; cur = 0
    for s, d in ev:
        if cur > 0 and last is not None:
            occ += s - last
        cur += d; last = s
    # number in flight at call start = starts before s minus ends <= s
    st = sorted(x[0] for x in al); en = sorted(x[1] for x in al)
    for s, e in al:
        inflight = bisect.bisect_right(st, s) - bisect.bisect_right(en, s)
        b = bucket(inflight)
        conc[b][0] += 1; conc[b][1] += e - s
    R['conc'] = dict(conc)
    R['occ_ns'] = occ
    # time-weighted mean number of threads inside allocator
    R['alloc_thread_ns'] = sum(e - s for s, e in al)

    # NVTX ranges per thread
    ranges = defaultdict(list)
    for s, e, t, txt, tid in c.execute(
            'select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?', (w0, w1)):
        ranges[t].append((s, -e, norm(txt if txt is not None else S.get(tid, ''))))
    attr_op = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    attr_leaf = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    tasks = defaultdict(lambda: [0, 0, 0, 0])  # sig -> ntasks, mallocs, frees, alloc_ns
    sync = defaultdict(lambda: defaultdict(int))
    syncn = defaultdict(int)
    synch = defaultdict(lambda: defaultdict(int))

    # GPU ops per (device,stream) for sync classification
    gops = defaultdict(list)
    for tbl in ('CUPTI_ACTIVITY_KIND_KERNEL', 'CUPTI_ACTIVITY_KIND_MEMCPY', 'CUPTI_ACTIVITY_KIND_MEMSET'):
        for s, e, d, st_ in c.execute(f'select start,end,deviceId,streamId from {tbl} where end>=? and start<=?', (w0 - 10**9, w1)):
            gops[st_].append((s, e))  # streamId is process-unique; SYNCHRONIZATION.deviceId is always 0
    gend = {}
    for key in gops:
        gops[key].sort(key=lambda x: x[1])
        gend[key] = [x[1] for x in gops[key]]
    syncmap = {}
    for corr, d, st_ in c.execute('select correlationId,deviceId,streamId from CUPTI_ACTIVITY_KIND_SYNCHRONIZATION where start>=? and end<=? and syncType=3', (w0, w1)):
        syncmap[corr] = st_

    for t, v in by_t.items():
        rs = sorted(ranges.get(t, []))
        ri = 0; stack = []
        for (s, e, _, n, corr) in v:
            while ri < len(rs) and rs[ri][0] <= s:
                stack = [x for x in stack if -x[1] > rs[ri][0]]
                stack.append(rs[ri]); ri += 1
            stack = [x for x in stack if -x[1] >= e]
            names = [x[2] for x in stack]
            if n in ALLOC or n in SYNCS:
                op = oplevel(names)
                leaf = names[-1] if names else '<none>'
            if n in ALLOC:
                idx = 0 if n == ALLOC[0] else 1
                a = attr_op[op][n]; a[0] += 1; a[1] += e - s
                a = attr_leaf[leaf][n]; a[0] += 1; a[1] += e - s
            elif n in SYNCS:
                dur = e - s
                key = syncmap.get(corr)
                cls = {}
                if key is None or key not in gend:
                    cls['nomatch'] = dur
                else:
                    ends = gend[key]; ops = gops[key]
                    j = bisect.bisect_right(ends, e + 5000) - 1
                    if j < 0 or ends[j] < s:
                        cls['nowork'] = dur
                    else:
                        le = min(ends[j], e)
                        # busy union of ops on this stream overlapping [s,le]
                        busy = 0; cur_e = s
                        jj = j
                        segs = []
                        while jj >= 0 and ends[jj] > s:
                            segs.append(ops[jj]); jj -= 1
                        segs.sort()
                        for (os_, oe) in segs:
                            a0 = max(os_, cur_e, s); a1 = min(oe, le)
                            if a1 > a0:
                                busy += a1 - a0
                            cur_e = max(cur_e, oe)
                        cls['busy'] = busy
                        cls['blocked'] = max(0, le - s - busy)
                        cls['tail'] = max(0, e - le)
                for kk, vv in cls.items():
                    sync[(n, op)][kk] += vv
                syncn[(n, op)] += 1
                main = 'nowork' if 'nowork' in cls else 'nomatch' if 'nomatch' in cls else 'work'
                hb = 'lt5' if dur < 5000 else 'lt20' if dur < 20000 else 'lt50' if dur < 50000 else 'lt200' if dur < 200000 else 'lt1000' if dur < 1000000 else 'ge1000'
                synch[(n, op, main)][hb] += 1
        # tasks: count task ranges and alloc calls inside them
    for t, rs in ranges.items():
        v = by_t.get(t, [])
        st = starts.get(t, [])
        for (s, ne, n) in rs:
            if n.startswith('Pipeline # Task #'):
                e = -ne
                sig = n.split('[', 1)[1].rstrip(']') if '[' in n else n
                i0 = bisect.bisect_left(st, s); i1 = bisect.bisect_right(st, e)
                tk = tasks[sig]; tk[0] += 1
                for r in v[i0:i1]:
                    if r[3] == ALLOC[0]:
                        tk[1] += 1; tk[3] += r[1] - r[0]
                    elif r[3] == ALLOC[1]:
                        tk[2] += 1; tk[3] += r[1] - r[0]
    R['attr_op'] = {k: {n: list(x) for n, x in v.items()} for k, v in attr_op.items()}
    R['attr_leaf'] = {k: {n: list(x) for n, x in v.items()} for k, v in attr_leaf.items()}
    R['tasks'] = {k: list(v) for k, v in tasks.items()}
    R['sync'] = {f'{a}|{b}': dict(v) for (a, b), v in sync.items()}
    R['syncn'] = {f'{a}|{b}': v for (a, b), v in syncn.items()}
    R['synch'] = {f'{a}|{b}|{m}': dict(v) for (a, b, m), v in synch.items()}
    return R


if __name__ == '__main__':
    mp, out = sys.argv[1], sys.argv[2]
    qs = {int(x) for x in sys.argv[3:]}
    res = {}
    for r in csv.DictReader(open(mp)):
        if r['iter'] != '2' or (qs and int(r['query']) not in qs):
            continue
        q = int(r['query'])
        res[q] = analyze(r['sqlite'], int(r['range_start_ns']), int(r['range_end_ns']))
        print('done', q, file=sys.stderr, flush=True)
    json.dump(res, open(out, 'w'))
