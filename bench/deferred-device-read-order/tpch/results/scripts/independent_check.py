import csv
import json
from collections import Counter, defaultdict
from decimal import Decimal
from pathlib import Path
from statistics import median

root = Path('/tmp/tpch-deferred-order-sf100-20261015')
oracles = {}
for q in [1, 6, 14, 19]:
    path = root / 'cpu' / f'q{q}' / f'q{q}-context_cold-0.tsv'
    if path.exists():
        oracles[f'q{q}'] = list(csv.reader(path.open(), delimiter='\t', quoting=csv.QUOTE_NONE))
validated = Counter()
for phase in ['smoke', 'diagnostic', 'timing', 'quent', 'nsys']:
    for path in (root / phase).glob('**/*.tsv'):
        q = path.stem.split('-')[0]
        if q not in oracles:
            continue
        got = list(csv.reader(path.open(), delimiter='\t', quoting=csv.QUOTE_NONE))
        expected = oracles[q]
        assert got[:2] == expected[:2] and len(got) == len(expected), path
        for actual_row, expected_row in zip(got[2:], expected[2:]):
            assert len(actual_row) == len(expected_row), path
            for kind, actual, wanted in zip(expected[1], actual_row, expected_row):
                if kind in ['FLOAT', 'DOUBLE'] and actual != '\\N' and wanted != '\\N':
                    a, b = Decimal(actual), Decimal(wanted)
                    assert a.is_finite() and b.is_finite(), path
                    assert abs(a-b) <= Decimal('1e-8') + Decimal('1e-10') * max(abs(a), abs(b)), (path, a, b)
                else:
                    assert actual == wanted, (path, actual, wanted)
        validated[phase] += 1

diags = defaultdict(Counter)
diag_files = 0
for path in (root / 'diagnostic').glob('*/*.json'):
    d = json.loads(path.read_text())
    if 'events' not in d:
        continue
    assert d['dropped'] == 0, path
    config, policy, query = path.parent.name.split('-')
    _, regime, _ = path.stem.split('-')
    key = '|'.join([config, policy, query, regime])
    c = diags[key]
    c['runs'] += 1
    diag_files += 1
    entries = [e for e in d['events'] if e['kind'] == 'entry' and e['role'] == 'own']
    counts = Counter(e['cohort'] for e in entries)
    assert max(counts.values(), default=0) <= 1, path
    c['cohorts'] += len(entries)
    c['unclaimed'] += d.get('unclaimed_open_awaits', 0)
    for e in entries:
        c['pending'] += e['pending']
        if e['open_siblings']:
            c['multi'] += 1
            c['multi_pending'] += e['pending']
            c['multi_sibling_pending'] += e['any_sibling_pending']
    for e in d['events']:
        if e['kind'] == 'wait':
            c[e['role']+'_waits'] += 1
            c[e['role']+'_wait_ns'] += e['duration_ns']
            assert e['duration_ns'] >= 0, path
            if e['role'] in ['own', 'sibling']:
                assert e['source_id'] and e['stream'] and e['device'] >= 0, path
        if e['kind'] == 'issue':
            c[e['role']+'_issues'] += 1

samples = defaultdict(list)
block_values = defaultdict(list)
for path in (root / 'timing').glob('*/timings.csv'):
    config, block, policy, query = path.parent.name.split('-')
    for row in csv.DictReader(path.open()):
        key = (config, policy, query, row['regime'])
        value = float(row['elapsed_ms'])
        assert value > 0, path
        samples[key].append(value)
        block_values[(*key, int(block[1:]))].append(value)
timings = []
for key, values in sorted(samples.items()):
    timings.append(dict(zip(['config', 'policy', 'query', 'regime'], key)) |
                   {'n': len(values), 'median_ms': median(values)})
pairs = []
for config in ['default', 'prefetch8']:
    for query in ['q1', 'q6', 'q14', 'q19']:
        for regime in ['fresh', 'retained']:
            deltas = []
            for s, o in [(0, 1), (3, 2), (5, 4), (6, 7)]:
                a = block_values.get((config, 'siblings_first', query, regime, s))
                b = block_values.get((config, 'own_first', query, regime, o))
                if a and b:
                    deltas.append((median(b)/median(a)-1)*100)
            if deltas:
                pairs.append({'config': config, 'query': query, 'regime': regime,
                              'own_delta_pct_by_pair': deltas, 'median_delta_pct': median(deltas)})
report = {'validated_results': dict(validated), 'oracle_queries': len(oracles),
          'diagnostic_files': diag_files, 'diagnostics': dict(diags),
          'timing_samples': sum(map(len, samples.values())), 'timings': timings, 'pairs': pairs}
(root / 'independent-review-check.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps({k: report[k] for k in ['validated_results', 'oracle_queries', 'diagnostic_files', 'timing_samples']}))
