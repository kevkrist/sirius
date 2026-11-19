#!/usr/bin/env python3
"""esites.py <out.json> <arm>=<telemetry label dir>[,<dir>...] [<arm>=...] -- <q> [<q> ...]
Per DF-consumer scan site (triple build PARTITION -> build CONCAT -> probe scan, taken from the tip reference
telemetry/baseline_tip by pipeline label), per arm, medians over hot iterations (session queries 2..N: warm and first hot dropped).
CPU only. Times are ms from the query's Executing event."""
import sys, os, glob, collections, statistics as st, json
sys.path.insert(0, '/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
REF = os.environ.get('REF', '/opt/dlami/nvme/dfperf/telemetry/baseline_tip')
G = 1e9
med = lambda v: st.median(v) if v else float('nan')

def pipes(s, it):
    qs = list(s['queries'].items()); qid, qev = qs[it]
    if 'Finished' not in qev or 'Executing' not in qev: return None, None
    t0 = qev['Executing']
    plans = [p for p, q in s['plans'].items() if q == qid]
    ops = {i: o for i, o in s['ops'].items() if o['plan'] in plans}
    byp = collections.defaultdict(list)
    for tid, ev in s['tasks'].items():
        pid = ev[0][3]['pipeline_uuid']['target']
        if pid not in ops: continue
        d = {'res': 0, 'mat': 0, 'inb': 0, 'tier': None, 'nres': 0, 'ttier': None}
        for seq, t, k, v in ev:
            t = (t - t0) / 1e6
            if k == 'Created': d['cr'] = t
            elif k == 'Reserving': d.setdefault('rs', t); d['res'] = v.get('requested_bytes', 0); d['mat'] = v.get('bytes_to_materialize', 0); d['nres'] += 1
            elif k == 'Preparing': d.setdefault('pr', t); d['inb'] = v.get('input_bytes', 0); d['tier'] = v.get('origin_tier'); d['ttier'] = v.get('target_tier')
            elif k == 'Computing': d.setdefault('c0', t)
            elif k == 'Finalizing': d['end'] = t; d['ok'] = v.get('success')
            elif k == 'Routing': d['gpu'] = v.get('preferred_device_id')
        if 'end' in d and 'cr' in d: byp[pid].append(d)
    out = {}
    for pid, ds in byp.items():
        out[ops[pid]['label']] = dict(lab=ops[pid]['label'], cr=min(d['cr'] for d in ds), end=max(d['end'] for d in ds), ds=ds)
    return out, (qev['Finished'] - t0) / 1e6

def ref_sites(q):
    sess = sorted(glob.glob(f'{REF}/q{q}/*'))[0]
    P, _ = pipes(qload.load(sess), 3)
    order = sorted(P.values(), key=lambda p: p['cr'])
    sites = []
    for i, p in enumerate(order):
        if i < 2 or not p['lab'].startswith('GPU_SCAN'): continue
        c, b = order[i - 1], order[i - 2]
        if not c['lab'].startswith('CONCAT') or not b['lab'].startswith('PARTITION'): continue
        hj = [x for x in order[i + 1:] if x['lab'].startswith('HASH_JOIN')][:1]
        sites.append(dict(b=b['lab'], c=c['lab'], s=p['lab'], hj=hj[0]['lab'] if hj else None, df='DYNAMIC_FILTER' in p['lab']))
    return sites

def measure(P, site):
    b, c, s = P.get(site['b']), P.get(site['c']), P.get(site['s'])
    if not (b and c and s): return None
    pub = b['end']
    cprep = [d.get('c0', d['end']) - d.get('pr', d['cr']) for d in c['ds']]
    cq = [d.get('pr', d['cr']) - d['cr'] for d in c['ds']]
    sd = s['ds']
    sdur = [d['end'] - d.get('pr', d['cr']) for d in sd]
    rw = [d.get('pr', d['cr']) - d['rs'] for d in sd + c['ds'] if 'rs' in d]
    hj = P.get(site['hj']) if site['hj'] else None
    r = dict(pub=pub, c_cr=c['cr'], c_start=min(d.get('pr', d['cr']) for d in c['ds']), c_end=c['end'], c_span=c['end'] - c['cr'],
             c_prep_med=med(cprep), c_prep_max=max(cprep), c_queue_max=max(cq), c_moved=sum(d['mat'] for d in c['ds']) / G,
             s_cr=s['cr'], s_first=min(d.get('pr', d['cr']) for d in sd), s_end=s['end'], s_n=len(sd),
             s_gap_cr=s['cr'] - pub, s_gap_first=min(d.get('pr', d['cr']) for d in sd) - pub,
             s_before_pub=sum(1 for d in sd if d.get('pr', d['cr']) < pub), s_created_before_pub=sum(1 for d in sd if d['cr'] < pub),
             s_chain=s['end'] - s['cr'], s_task_med=med(sdur), s_task_sum=sum(sdur),
             site_span=max(c['end'], s['end']) - pub, resv_wait_max=max(rw) if rw else 0.0,
             retries=sum(1 for d in sd + c['ds'] if d['nres'] > 1), not_gpu=sum(1 for d in sd + c['ds'] if d.get('ttier') not in (None, 'GPU')),
             failed=sum(1 for d in sd + c['ds'] if d.get('ok') is False),
             overlap=max(0.0, min(c['end'], s['end']) - max(c['c_start'] if 'c_start' in c else min(d.get('pr', d['cr']) for d in c['ds']), s['cr'])))
    if hj:
        r['hj_cr'] = hj['cr']; r['hj_after_build'] = hj['cr'] - c['end']; r['hj_after_scan'] = hj['cr'] - s['end']; r['hj_end'] = hj['end']
    return r

def main():
    out = sys.argv[1]; i = sys.argv.index('--'); arms = collections.OrderedDict()
    for a in sys.argv[2:i]:
        k, v = a.split('='); arms[k] = v.split(',')
    qs = sys.argv[i + 1:]
    res = {}
    for q in qs:
        sites = ref_sites(q); res[q] = {}
        for arm, dirs in arms.items():
            walls = []; per = collections.defaultdict(list)
            for d in dirs:
                for sess in sorted(glob.glob(f'{d}/q{q}/*')):
                    s = qload.load(sess); n = len(s['queries'])
                    for it in range(2, n):
                        P, wall = pipes(s, it)
                        if P is None: continue
                        walls.append(wall)
                        for si, site in enumerate(sites):
                            m = measure(P, site)
                            if m: per[si].append(m)
            res[q][arm] = dict(wall=med(walls), n=len(walls), sites={})
            for si, site in enumerate(sites):
                ms = per[si]
                if not ms: continue
                agg = {k: med([m[k] for m in ms if k in m]) for k in ms[0]}
                for k in ('s_before_pub', 's_created_before_pub', 'retries', 'not_gpu', 'failed'): agg[k] = sum(m[k] for m in ms)
                agg['resv_wait_max'] = max(m['resv_wait_max'] for m in ms)
                res[q][arm]['sites'][si] = agg
        res[q]['_sites'] = sites
    json.dump(res, open(out, 'w'), indent=1)
    names = list(arms)
    f = lambda x: '-' if x is None or x != x else f'{x:.1f}'
    print('| q | site (PARTITION / CONCAT / scan) | DF | arm | n | wall | pub @ | CONCAT start-pub | CONCAT prep med | CONCAT end-pub | scan created-pub | scan first start-pub | scan n | scan task med | scan chain | scan end-pub | site span (max end - pub) | overlap | HJ created - build end | HJ created - scan end | scan tasks started/created before pub | resv wait max | retries / non-GPU / failed |')
    print('|' + '---|' * 23)
    for q in qs:
        for si, site in enumerate(res[q]['_sites']):
            for arm in names:
                a = res[q][arm]; m = a['sites'].get(si)
                if not m: continue
                if m['c_span'] < 5 and arm == names[0] and not os.environ.get('ALL'): break
                lab = f"{site['b'].split(' ')[0]} / {site['c'].split(' ')[0]} / {site['s'].split(' ')[0]}"
                print(f"| {q} | {lab} | {'y' if site['df'] else 'n'} | {arm} | {a['n']} | {f(a['wall'])} | {f(m['pub'])} | {f(m['c_start']-m['pub'])} | {f(m['c_prep_med'])} | {f(m['c_end']-m['pub'])} | {f(m['s_gap_cr'])} | {f(m['s_gap_first'])} | {m['s_n']:.0f} | {f(m['s_task_med'])} | {f(m['s_chain'])} | {f(m['s_end']-m['pub'])} | {f(m['site_span'])} | {f(m['overlap'])} | {f(m.get('hj_after_build'))} | {f(m.get('hj_after_scan'))} | {m['s_before_pub']} / {m['s_created_before_pub']} | {m['resv_wait_max']:.2f} | {m['retries']} / {m['not_gpu']} / {m['failed']} |")

main()
