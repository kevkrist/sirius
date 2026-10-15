"""Validate every result and summarize independent blocks and diagnostic events."""
import csv
from collections import defaultdict
from decimal import Decimal
import json
from pathlib import Path
import statistics

OUT = Path('/tmp/tpch-deferred-order-sf100-20261015')


def read_tsv(path):
    lines = path.read_text().splitlines()
    return [line.split('\t') for line in lines]


def equal_result(expected, actual):
    if expected[:2] != actual[:2] or len(expected) != len(actual):
        return False
    for left, right in zip(expected[2:], actual[2:]):
        if len(left) != len(right):
            return False
        for column, (a, b) in enumerate(zip(left, right)):
            if a == b:
                continue
            if expected[1][column] not in ['DOUBLE', 'FLOAT'] or '\\N' in [a, b]:
                return False
            x, y = Decimal(a), Decimal(b)
            if abs(x - y) > Decimal('1e-10') * max(abs(x), abs(y)) + Decimal('1e-8'):
                return False
    return True


def write_csv(path, rows):
    if not rows:
        return
    with path.open('w') as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def completed_files(category, pattern):
    for record in sorted((OUT / category).glob('*.run.json')):
        if json.loads(record.read_text())['returncode'] != 0:
            continue
        directory = record.parent / record.name.removesuffix('.run.json')
        yield from sorted(directory.glob(pattern))


def main():
    oracles = {f'q{q}': read_tsv(OUT / 'cpu' / f'q{q}' / f'q{q}-context_cold-0.tsv')
               for q in [1, 6, 14, 19]}
    verified = 0
    for category in ['smoke', 'diagnostic', 'timing', 'quent', 'nsys']:
        for path in completed_files(category, '*.tsv'):
            query = path.stem.split('-')[0]
            if query not in oracles:
                continue
            if not equal_result(oracles[query], read_tsv(path)):
                raise RuntimeError(f'CPU mismatch: {path}')
            verified += 1

    samples = []
    groups = defaultdict(list)
    blocks = defaultdict(list)
    for path in completed_files('timing', 'timings.csv'):
        config, block, policy, query = path.parent.name.split('-')
        for row in csv.DictReader(path.open()):
            sample = {'config': config, 'block': int(block[1:]), 'policy': policy,
                      **row, 'path': str(path.relative_to(OUT))}
            samples.append(sample)
            value = float(row['elapsed_ms'])
            key = (config, policy, query, row['regime'])
            groups[key].append(value)
            blocks[(*key, int(block[1:]))].append(value)
    summary = []
    for key, values in sorted(groups.items()):
        block_medians = [statistics.median(v) for k, v in blocks.items() if k[:4] == key]
        median = statistics.median(values)
        summary.append(dict(zip(['config', 'policy', 'query', 'regime'], key)) | {
            'samples': len(values), 'median_ms': median, 'min_ms': min(values), 'max_ms': max(values),
            'block_median_min_ms': min(block_medians), 'block_median_max_ms': max(block_medians),
            'block_median_range_percent': (max(block_medians)-min(block_medians))/statistics.median(block_medians)*100})
    pairs = []
    for config in ['default', 'prefetch8']:
        for query in ['q1', 'q6', 'q14', 'q19']:
            for regime in ['fresh', 'retained', 'context_cold']:
                for pair, (s, o) in enumerate([(0,1),(3,2),(5,4),(6,7)]):
                    sv = blocks.get((config, 'siblings_first', query, regime, s))
                    ov = blocks.get((config, 'own_first', query, regime, o))
                    if sv and ov:
                        sm, om = statistics.median(sv), statistics.median(ov)
                        pairs.append({'config': config, 'query': query, 'regime': regime, 'pair': pair,
                                      'siblings_ms': sm, 'own_ms': om, 'own_delta_ms': om-sm,
                                      'own_delta_percent': (om-sm)/sm*100})
    diagnostics = []
    for path in completed_files('diagnostic', '*.json'):
        data = json.loads(path.read_text())
        if 'events' not in data:
            continue
        if data['dropped']:
            raise RuntimeError(f'Dropped diagnostics: {path}')
        config, policy, query = path.parent.name.split('-')
        _, regime, iteration = path.stem.split('-')
        events = data['events']
        entries = [e for e in events if e['kind'] == 'entry' and e['role'] == 'own']
        multi = [e for e in entries if e['open_siblings'] > 0]
        own_waits = [e for e in events if e['kind'] == 'wait' and e['role'] == 'own']
        sibling_waits = [e for e in events if e['kind'] == 'wait' and e['role'] == 'sibling']
        multi_ids = {e['cohort'] for e in multi}
        diagnostics.append({'config': config, 'policy': policy, 'query': query,
                            'regime': regime, 'iteration': int(iteration),
                            'first_awaits': len(entries), 'multi_source_first_awaits': len(multi),
                            'a_pending_at_entry': sum(e['pending'] for e in entries),
                            'multi_a_pending_at_entry': sum(e['pending'] for e in multi),
                            'multi_any_sibling_pending_at_entry': sum(e['any_sibling_pending'] for e in multi),
                            'multi_a_and_sibling_pending_at_entry': sum(e['pending'] and e['any_sibling_pending'] for e in multi),
                            'multi_only_a_pending_at_entry': sum(e['pending'] and not e['any_sibling_pending'] for e in multi),
                            'own_waits': len(own_waits), 'multi_own_waits': sum(e['cohort'] in multi_ids for e in own_waits),
                            'own_wait_sum_ms': sum(e['duration_ns'] for e in own_waits)/1e6,
                            'own_wait_max_ms': max([e['duration_ns'] for e in own_waits], default=0)/1e6,
                            'sibling_waits': len(sibling_waits),
                            'sibling_wait_sum_ms': sum(e['duration_ns'] for e in sibling_waits)/1e6,
                            'actual_own_issues': sum(e['kind']=='issue' and e['role']=='own' for e in events),
                            'actual_sibling_issues': sum(e['kind']=='issue' and e['role']=='sibling' for e in events),
                            'already_issued_awaits': data['already_issued_awaits'],
                            'unclaimed_open_awaits': data.get('unclaimed_open_awaits', 0),
                            'dropped': data['dropped'], 'path': str(path.relative_to(OUT))})
    write_csv(OUT / 'timing_samples.csv', samples)
    write_csv(OUT / 'timing_summary.csv', summary)
    write_csv(OUT / 'paired_blocks.csv', pairs)
    write_csv(OUT / 'diagnostic_summary.csv', diagnostics)
    (OUT / 'validation.json').write_text(json.dumps({'verified_gpu_results': verified, 'cpu_oracles': len(oracles),
        'float_tolerance': 'abs <= 1e-8 + 1e-10 * max(abs(cpu), abs(gpu)); other types exact',
        'timing_samples': len(samples), 'diagnostic_executions': len(diagnostics)}, indent=2)+'\n')
    print(f'Validated {verified} result files; {len(samples)} timing samples; {len(diagnostics)} diagnostic executions.')


if __name__ == '__main__':
    main()
