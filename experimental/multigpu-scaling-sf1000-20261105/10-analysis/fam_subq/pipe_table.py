#!/usr/bin/env python3
"""Per-pipeline Quent accounting for one query label in two runs.
usage: pipe_table.py <quentA> <quentB> <label e.g. tpch_q15_iter3>"""
import sys, pandas as pd
def load(d, lab):
    t = pd.read_csv(f"{d}/tasks.csv"); t = t[t['query'] == lab].copy()
    p = pd.read_csv(f"{d}/pipelines.csv"); p = p[p['query'] == lab]
    q = pd.read_csv(f"{d}/queries.csv"); q = q[q['query'] == lab].iloc[0]
    o = pd.read_csv(f"{d}/task_ops.csv"); o = o[o['query'] == lab].copy()
    t = t.merge(p[['pipeline', 'label', 'type_name']], on='pipeline', how='left')
    t0 = q['init']
    t['prep_ms'] = (t['first_compute'] - t['preparing']) / 1e6
    t['comp_ms'] = (t['finalize'] - t['first_compute']) / 1e6
    t['s'] = (t['preparing'] - t0) / 1e6; t['e'] = (t['finalize'] - t0) / 1e6
    t['wait_ms'] = (t['preparing'] - t['created']) / 1e6
    return t, q, o
A, qa, oa = load(sys.argv[1], sys.argv[3]); B, qb, ob = load(sys.argv[2], sys.argv[3])
print(f"wall A={qa['wall_ms']:.1f} B={qb['wall_ms']:.1f}")
def summ(t):
    g = t.groupby(['type_name', 'label'])
    r = g.agg(n=('task', 'size'), prepH=('prep_ms', lambda x: x[t.loc[x.index, 'origin'] == 'HOST'].sum()),
              prepG=('prep_ms', lambda x: x[t.loc[x.index, 'origin'] != 'HOST'].sum()), comp=('comp_ms', 'sum'),
              s=('s', 'min'), e=('e', 'max'), inGB=('in_bytes', lambda x: x.sum() / 1e9))
    r['pid'] = [int(x.split()[-1]) for x in r.index.get_level_values(0)]
    return r.reset_index().sort_values('pid')
sa, sb = summ(A), summ(B)
m = sa.merge(sb, on=['pid'], suffixes=('_1', '_4'), how='outer')
pd.set_option('display.width', 250); pd.set_option('display.max_colwidth', 60)
cols = ['pid', 'label_1', 'n_1', 'n_4', 'prepH_1', 'prepH_4', 'prepG_1', 'prepG_4', 'comp_1', 'comp_4', 's_1', 'e_1', 's_4', 'e_4', 'inGB_1', 'inGB_4']
print(m[cols].round(1).to_string(index=False))
print("TOTAL thread-ms A: prepH %.0f prepG %.0f comp %.0f | B: prepH %.0f prepG %.0f comp %.0f" % (
    sa.prepH.sum(), sa.prepG.sum(), sa.comp.sum(), sb.prepH.sum(), sb.prepG.sum(), sb.comp.sum()))
# per op compute
for nm, o, t in (('A', oa, A), ('B', ob, B)):
    o = o.copy(); o['ms'] = (o['end'] - o['start']) / 1e6
    pl = t.drop_duplicates('pipeline')[['pipeline']]
    print(nm, 'op compute ms:', o.groupby('operator_id')['ms'].agg(['count', 'sum', 'median']).round(2).T.to_dict())
