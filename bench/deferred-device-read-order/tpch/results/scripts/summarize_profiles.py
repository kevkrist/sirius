#!/usr/bin/env python3
"""Summarize completed attribution traces; never launch GPU work."""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re
import sqlite3


def artifact(path, root):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'path': str(path.relative_to(root)), 'bytes': path.stat().st_size,
            'sha256': digest.hexdigest()}


def timing_rows(root, kind, identity):
    path = root / kind / identity / 'timings.csv'
    return list(csv.DictReader(path.open())) if path.exists() else []


def identity_of(row):
    return f"{row['query']}-{row['regime']}-{row['iteration']}"


def summarize_nsys(path, root):
    identity, ordinal = path.stem.rsplit('.', 1)
    rows = timing_rows(root, 'nsys', identity)
    family_paths = list(path.parent.glob(identity + '.*.sqlite'))
    ordinal_mapping_valid = len(rows) == 5 and len(family_paths) == 5
    selected = [row for row in rows if int(row['capture_ordinal']) + 1 == int(ordinal)]
    if len(selected) != 1:
        raise ValueError(f'Capture ordinal has no timing row: {path}')
    with sqlite3.connect(f'file:{path}?mode=ro', uri=True) as conn:
        tables = {x[0] for x in conn.execute("select name from sqlite_master where type='table'")}
        kernels = conn.execute('select count(*),sum(end-start),min(start),max(end) from CUPTI_ACTIVITY_KIND_KERNEL').fetchone()
        names = conn.execute('select s.value,count(*),sum(k.end-k.start) from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on k.shortName=s.id group by s.value order by 3 desc').fetchall()
        copies = conn.execute('select coalesce(e.label,e.name,cast(c.copyKind as text)),count(*),sum(c.bytes),sum(c.end-c.start) from CUPTI_ACTIVITY_KIND_MEMCPY c left join ENUM_CUDA_MEMCPY_OPER e on c.copyKind=e.id group by c.copyKind').fetchall()
        nvtx = conn.execute('select coalesce(n.text,s.value),count(*),min(start),max(end) from NVTX_EVENTS n left join StringIds s on n.textId=s.id group by 1').fetchall()
        semantic = [{'name': name, 'count': count, 'start_ns': start, 'end_ns': end}
                    for name, count, start, end in nvtx if name and (name.startswith('Pipeline ') or re.search('spill|downgrade|read_parquet|byte_ranges', name, re.I))]
        explicit_labels = [name for name, *_ in nvtx if name and re.search(r'q(?:1|6|14|19)-(?:context_cold|fresh|retained)-', name)]
        total_nvtx = sum(x[1] for x in nvtx)
    report = path.with_suffix('.nsys-rep')
    return {'sqlite': artifact(path, root), 'report': artifact(report, root) if report.exists() else None,
            'identity': identity, 'report_ordinal': int(ordinal),
            'runner_capture_ordinal': int(selected[0]['capture_ordinal']) if ordinal_mapping_valid else None,
            'query_identity': identity_of(selected[0]) if ordinal_mapping_valid else None,
            'runner_timing_row': selected[0] if ordinal_mapping_valid else None,
            'family_runner_timing_rows': rows,
            'mapping_basis': ('Five cudaProfilerApi repeat captures match five runner capture ordinals; suffix = ordinal + 1' if ordinal_mapping_valid else 'Ambiguous per-execution alignment: four capture files for five query executions; no explicit query labels in NVTX. Query family is known from invocation. Do not assign individual cache regimes by suffix.'),
            'explicit_query_labels_in_nvtx': explicit_labels,
            'kernel_count': kernels[0], 'kernel_summed_duration_ns': kernels[1],
            'kernel_first_start_ns': kernels[2], 'kernel_last_end_ns': kernels[3],
            'kernels_by_short_name': [{'name': n, 'count': c, 'summed_duration_ns': d} for n,c,d in names],
            'memcpy_count': sum(x[1] for x in copies),
            'copies_by_kind': [{'kind': n, 'count': c, 'bytes': b, 'summed_duration_ns': d} for n,c,b,d in copies],
            'nvtx_count': total_nvtx, 'semantic_nvtx_ranges': semantic}


def records(directory, entity):
    for path in sorted(directory.glob(f'*/{entity}/*.ndjson')):
        for line in path.read_text().splitlines():
            if line.strip():
                yield json.loads(line)


def summarize_quent(directory, root):
    identity = directory.name.removesuffix('-events')
    rows = timing_rows(root, 'quent', identity)
    queries = {}
    for event in records(directory, 'query'):
        state = event['data'].get('state')
        if isinstance(state, dict) and 'Init' in state:
            queries[event['id']] = {'query_identity': state['Init']['instance_name'],
                                    'start_ns': event['timestamp'], 'exit_ns': None, 'operators': [], 'task_states': collections.Counter()}
        elif state == 'Exit' and event['id'] in queries:
            queries[event['id']]['exit_ns'] = event['timestamp']
    plans = {}
    for event in records(directory, 'plan'):
        declaration = event['data'].get('Declaration', {})
        if declaration:
            plans[event['id']] = declaration['parent']['query_id']
    operators = {}
    for event in records(directory, 'operator'):
        declaration = event['data'].get('Declaration', {})
        if declaration:
            qid = plans.get(declaration['plan_id'])
            operator = {'operator_id': event['id'], 'pipeline': declaration['type_name'],
                        'path': declaration['instance_name'], 'created_tasks': 0, 'exited_tasks': 0,
                        'task_states': collections.Counter()}
            operators[event['id']] = (qid, operator)
            if qid in queries:
                queries[qid]['operators'].append(operator)
    tasks = {}
    for event in records(directory, 'task'):
        state = event['data'].get('state')
        name = next(iter(state)) if isinstance(state, dict) else state
        if name == 'Created':
            tasks[event['id']] = state['Created']['pipeline_uuid']
        pair = operators.get(tasks.get(event['id']))
        if pair:
            qid, operator = pair
            operator['task_states'][name] += 1
            operator['created_tasks'] += name == 'Created'
            operator['exited_tasks'] += name == 'Exit'
            if qid in queries:
                queries[qid]['task_states'][name] += 1
    result = list(queries.values())
    expected = [identity_of(row) for row in rows]
    actual = [row['query_identity'] for row in result]
    complete = len(rows) == 5 and expected == actual and all(row['exit_ns'] is not None for row in result)
    files = [artifact(path, root) for path in sorted(directory.rglob('*')) if path.is_file()]
    return {'identity': identity, 'complete_five_queries': complete, 'runner_query_identities': expected,
            'queries': result, 'artifacts': files, 'total_bytes': sum(x['bytes'] for x in files)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    root = args.root.resolve()
    result = {'scope': 'Separate attribution runs, not paired timing samples. Durations are inclusive sums and may overlap; they are not end-to-end contributions or removable headroom. NVTX spill/downgrade ranges alone do not prove bytes spilled or CPU fallback.',
              'nsys': [], 'quent': [], 'incomplete': []}
    for path in sorted((root / 'nsys/profiles').glob('*.sqlite')):
        try:
            result['nsys'].append(summarize_nsys(path, root))
        except (sqlite3.Error, ValueError) as error:
            result['incomplete'].append({'path': str(path.relative_to(root)), 'reason': str(error)})
    for directory in sorted((root / 'quent').glob('*-events')):
        try:
            summary = summarize_quent(directory, root)
            result['quent'].append(summary)
            if not summary['complete_five_queries']:
                result['incomplete'].append({'path': str(directory.relative_to(root)), 'reason': 'Expected five matching query labels with Exit state'})
        except (json.JSONDecodeError, OSError) as error:
            result['incomplete'].append({'path': str(directory.relative_to(root)), 'reason': str(error)})
    comparisons = []
    for query in [1, 6, 14, 19]:
        sides = {}
        for policy in ['siblings_first', 'own_first']:
            trace = next((x for x in result['quent'] if x['identity'] == f'prefetch8-{policy}-q{query}'), None)
            if trace and trace['complete_five_queries']:
                sides[policy] = {x['query_identity']: sorted((op['pipeline'], op['path'], op['created_tasks'], op['exited_tasks']) for op in x['operators']) for x in trace['queries']}
        comparisons.append({'query': f'q{query}', 'both_complete': len(sides) == 2,
                            'same_operator_paths_and_task_counts': sides.get('siblings_first') == sides.get('own_first') if len(sides) == 2 else None})
    result['quent_policy_consistency'] = comparisons
    result['nsys_capture_alignment'] = []
    for query in [1, 6, 14, 19]:
        identity = f'prefetch8-siblings_first-q{query}'
        captures = [x for x in result['nsys'] if x['identity'] == identity]
        result['nsys_capture_alignment'].append({
            'query': f'q{query}', 'capture_files': len(captures),
            'runner_executions': len(timing_rows(root, 'nsys', identity)),
            'per_execution_alignment_unambiguous_by_ordinal': len(captures) == 5,
            'gpu_scan_counts_by_capture': [sum(e['count'] for e in x['semantic_nvtx_ranges'] if 'GPU_SCAN (id=' in e['name']) for x in captures],
            'interpretation': ('Five ordered captures for five executions.' if len(captures) == 5 else 'Second capture has twice the GPU_SCAN ranges and approximately twice the kernels/copies of the others, consistent with merged adjacent executions. Exact iteration mapping is not established from explicit query labels.')})
    result['complete'] = (all(x['capture_files'] >= 1 and x['runner_executions'] == 5 for x in result['nsys_capture_alignment']) and len(result['quent']) == 8 and not result['incomplete'])
    result['complete_definition'] = 'Four query families have CUDA captures and five runner rows each; eight Quent traces have five complete matching labels each. This does not assert one Nsight report per query execution.'
    output = root / 'profile_summary.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'nsys_reports': len(result['nsys']), 'quent_traces': len(result['quent']), 'complete': result['complete'], 'incomplete': result['incomplete'], 'consistency': comparisons}, indent=2))


if __name__ == '__main__':
    main()
