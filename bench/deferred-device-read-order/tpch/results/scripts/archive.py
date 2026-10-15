"""Retain compact, independently checkable evidence without binaries/profiler blobs."""
import gzip
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path('/data/sirius_optblind/wt/landing-f1-deferred-vectored-reads')
RUN = Path('/tmp/tpch-deferred-order-sf100-20261015')
BUILD = Path('/tmp/tpch-deferred-order-build')
DEST = ROOT / 'bench/deferred-device-read-order/tpch/results'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    DEST.mkdir(exist_ok=True)
    for name in ['CONTRACT.md', 'config.yaml', 'prefetch8.yaml', 'manifest.json',
                 'dataset_metadata.json', 'cpu-oracle-provenance.json', 'validation.json',
                 'timing_samples.csv', 'timing_summary.csv', 'paired_blocks.csv',
                 'diagnostic_summary.csv', 'profile_summary.json', 'tables.md', 'dirty.patch',
                 'independent-review-check.json']:
        path = RUN / name
        if path.exists():
            shutil.copyfile(path, DEST / name)
    for folder in ['workloads', 'profile-configs']:
        if (RUN / folder).exists():
            shutil.copytree(RUN / folder, DEST / folder, dirs_exist_ok=True)
    # Preserve the full profile analysis byte-for-byte, with a small readable index.
    full_profile = RUN / 'profile_summary.json'
    with gzip.open(DEST / 'profile_details.json.gz', 'wb') as output:
        output.write(full_profile.read_bytes())
    profile = json.loads(full_profile.read_text())
    compact = {key: value for key, value in profile.items() if key not in ['nsys', 'quent']}
    compact['full_details'] = {'path': 'profile_details.json.gz', 'uncompressed_sha256': sha(full_profile)}
    compact['nsys'] = [{key: row[key] for key in ['identity', 'sqlite', 'report', 'kernel_count',
                                                'query_identity', 'mapping_basis']} for row in profile['nsys']]
    compact['quent'] = [{key: row[key] for key in ['identity', 'complete_five_queries',
                                                 'runner_query_identities', 'total_bytes']} for row in profile['quent']]
    (DEST / 'profile_summary.json').write_text(json.dumps(compact, indent=2)+'\n')
    scripts = DEST / 'scripts'
    scripts.mkdir(exist_ok=True)
    for name in ['prepare.py', 'run_matrix.py', 'run_profiles.py', 'analyze.py', 'archive.py', 'dataset_metadata.py',
                 'make_tables.py', 'summarize_profiles.py']:
        shutil.copyfile(RUN / name, scripts / name)
    shutil.copyfile('/tmp/tpch-deferred-order-independent-check.py', scripts / 'independent_check.py')
    shutil.copyfile('/tmp/tpch-deferred-order-arch.md', DEST / 'architecture-reading-and-analysis.md')
    shutil.copyfile('/tmp/tpch-deferred-order-plan-review.md', DEST / 'plan-review.md')
    calibration = DEST / 'calibration'
    calibration.mkdir(exist_ok=True)
    old = Path('/tmp/tpch-deferred-order-20261015')
    for name in ['config.yaml', 'manifest.json', 'CONTRACT.md']:
        shutil.copyfile(old / name, calibration / name)
    generated = DEST / 'build'
    generated.mkdir(exist_ok=True)
    for path in sorted(BUILD.iterdir()):
        if path.suffix in ['.cpp', '.diff', '.json']:
            shutil.copyfile(path, generated / path.name)
    map_rows = []
    for path in sorted(BUILD.glob('*.map')):
        lines = path.read_text().splitlines()
        selected = []
        for index, line in enumerate(lines):
            if 'sirius::io::deferred_device_batch::await(unsigned long)' in line:
                selected.extend(lines[max(0, index-3):index+2])
        map_rows.append({'map': str(path), 'sha256': sha(path), 'await_excerpt': selected})
    (generated / 'link-map-evidence.json').write_text(json.dumps(map_rows, indent=2)+'\n')
    manifests, results, events, logs = [], [], [], []
    for category in ['cpu', 'diagnostic', 'timing', 'quent', 'nsys']:
        for record in sorted((RUN / category).glob('*.run.json')):
            metadata = json.loads(record.read_text())
            directory = record.parent / record.name.removesuffix('.run.json')
            identity = str(directory.relative_to(RUN))
            manifests.append({'identity': identity, **metadata})
            log = directory.with_suffix('.log')
            if log.exists():
                logs.append({'identity': identity, 'text': log.read_text(errors='replace')})
            if metadata['returncode'] != 0:
                continue
            for result in sorted(directory.glob('*.tsv')):
                results.append({'identity': str(result.relative_to(RUN)), 'text': result.read_text(), 'sha256': sha(result)})
            for event in sorted(directory.glob('*.json')):
                data = json.loads(event.read_text())
                if 'events' in data:
                    events.append({'identity': str(event.relative_to(RUN)), 'data': data})
    (DEST / 'run-manifests.json').write_text(json.dumps(manifests, indent=2)+'\n')
    for name, rows in [('query-results', results), ('diagnostic-events', events), ('process-logs', logs)]:
        with gzip.open(DEST / (name+'.jsonl.gz'), 'wt') as output:
            for row in rows:
                output.write(json.dumps(row)+'\n')
    with gzip.open(DEST / 'quent-events.jsonl.gz', 'wt') as output:
        for path in sorted((RUN / 'quent').rglob('*')):
            if path.suffix in ['.ndjson', '.qmi']:
                output.write(json.dumps({'identity': str(path.relative_to(RUN)),
                                         'text': path.read_text(), 'sha256': sha(path)})+'\n')
    hashes = {str(p.relative_to(DEST)): sha(p) for p in sorted(DEST.rglob('*'))
              if p.is_file() and p.name != 'checksums.json'}
    (DEST / 'checksums.json').write_text(json.dumps(hashes, indent=2)+'\n')
    print(f'Archived {len(results)} query results, {len(events)} diagnostic event sets, {len(manifests)} run manifests.')


if __name__ == '__main__':
    main()
