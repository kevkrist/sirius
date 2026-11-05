import csv, collections, statistics as st, sys
d = sys.argv[1]
lab = {r['pipeline']: r['label'] for r in csv.DictReader(open(d + '/pipelines.csv'))}
T = collections.defaultdict(list)
for r in csv.DictReader(open(d + '/tasks.csv')):
    if r['query'] in ('tpch_q1_iter2', 'tpch_q1_iter3', 'tpch_q1_iter4') and lab.get(r['pipeline'], '').startswith('GPU_SCAN(0)'):
        T[r['query']].append(r)
for q, rs in sorted(T.items()):
    t0 = min(int(r['created']) for r in rs)
    g = collections.defaultdict(list)
    for r in rs:
        g[r['gpu']].append(r)
    ends = {k: (max(int(r['finalize']) for r in v) - t0) / 1e6 for k, v in g.items()}
    cnt = {k: len(v) for k, v in g.items()}
    busy = {k: sum(int(r['finalize']) - int(r['preparing']) for r in v) / 1e9 for k, v in g.items()}
    print(q, 'cnt', dict(sorted(cnt.items())), 'ends', {k: round(v, 1) for k, v in sorted(ends.items())},
          'max-mean %.1f' % (max(ends.values()) - st.mean(ends.values())),
          'thread-s', {k: round(v, 2) for k, v in sorted(busy.items())})
