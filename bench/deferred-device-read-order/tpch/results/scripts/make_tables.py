import csv
from pathlib import Path

OUT = Path('/tmp/tpch-deferred-order-sf100-20261015')
summary = list(csv.DictReader((OUT / 'timing_summary.csv').open()))
pairs = list(csv.DictReader((OUT / 'paired_blocks.csv').open()))
diag = list(csv.DictReader((OUT / 'diagnostic_summary.csv').open()))
lookup = {(r['config'], r['policy'], r['query'], r['regime']): r for r in summary}
lines = []
for config in ['default', 'prefetch8']:
    lines += [f'## {config}', '', '| Query | Fresh S / O ms | O change | Retained S / O ms | O change |', '| --- | ---: | ---: | ---: | ---: |']
    totals = {'fresh': [0., 0.], 'retained': [0., 0.]}
    for q in ['q1', 'q6', 'q14', 'q19']:
        cells = [q]
        for regime in ['fresh', 'retained']:
            s = lookup[(config, 'siblings_first', q, regime)]
            o = lookup[(config, 'own_first', q, regime)]
            assert int(s['samples']) == int(o['samples']) == 6
            sm, om = float(s['median_ms']), float(o['median_ms'])
            totals[regime][0] += sm
            totals[regime][1] += om
            cells += [f'{sm:.2f} / {om:.2f}', f'{(om/sm-1)*100:+.2f}%']
        lines.append('| ' + ' | '.join(cells) + ' |')
    cells = ['Sum']
    for regime in ['fresh', 'retained']:
        sm, om = totals[regime]
        cells += [f'{sm:.2f} / {om:.2f}', f'{(om/sm-1)*100:+.2f}%']
    lines += ['| '+' | '.join(cells)+' |', '', '| Query/regime | O change pair 1 / pair 2 | Baseline block-median range |', '| --- | ---: | ---: |']
    for q in ['q1', 'q6', 'q14', 'q19']:
        for regime in ['fresh', 'retained']:
            p = [r for r in pairs if r['config']==config and r['query']==q and r['regime']==regime]
            assert len(p)==2
            s = lookup[(config, 'siblings_first', q, regime)]
            cells = [f'{q}/{regime}', ' / '.join(f"{float(r['own_delta_percent']):+.2f}%" for r in p),
                     f"{float(s['block_median_range_percent']):.2f}%"]
            lines.append('| '+' | '.join(cells)+' |')
    lines.append('')
lines += ['## First-wait diagnostic counts', '', '| Query | A pending/all first groups | A pending/multi-source groups | A and sibling pending | A-only pending |', '| --- | ---: | ---: | ---: | ---: |']
for q in ['q1','q6','q14','q19']:
    rows = [r for r in diag if r['config']=='prefetch8' and r['policy']=='siblings_first' and r['query']==q and r['regime']=='fresh']
    def total(key): return sum(int(r[key]) for r in rows)
    lines.append(f"| {q} | {total('a_pending_at_entry')}/{total('first_awaits')} | {total('multi_a_pending_at_entry')}/{total('multi_source_first_awaits')} | {total('multi_a_and_sibling_pending_at_entry')} | {total('multi_only_a_pending_at_entry')} |")
(OUT / 'tables.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
