import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import yaml

ROOT = Path('/data/sirius_optblind/wt/landing-f1-deferred-vectored-reads')
OUT = Path('/tmp/tpch-deferred-order-sf100-20261015')
DATA = Path('/data/tpch_parquet_sf100')
QUERIES = [1, 6, 14, 19]
TABLES = ['customer', 'lineitem', 'nation', 'orders', 'part', 'partsupp', 'region', 'supplier']

base = yaml.safe_load((OUT / 'config.yaml').read_text())
active = yaml.safe_load((OUT / 'config.yaml').read_text())
active['sirius']['executor']['scan_manager']['max_readahead_scans'] = 8
active['sirius']['executor']['scan_manager']['readahead_strategy'] = 'opportunistic'
(OUT / 'prefetch8.yaml').write_text(yaml.safe_dump(active, sort_keys=False))
(OUT / 'spill').mkdir(exist_ok=True)
setup = '\n'.join(
    f"CREATE VIEW {table} AS SELECT * FROM read_parquet('{DATA / table}/*.parquet');"
    for table in TABLES
)
for query in QUERIES:
    target = OUT / 'workloads' / f'q{query}'
    target.mkdir(parents=True, exist_ok=True)
    (target / 'setup.sql').write_text(setup + '\n')
    source = ROOT / 'test/tpch_performance/tpch_queries/orig' / f'q{query}.sql'
    shutil.copyfile(source, target / 'query.sql')
    (target / 'queries.tsv').write_text(f'q{query}\tquery.sql\n')

manifest = {
    'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    'dataset': str(DATA),
    'files': [{'path': str(p), 'bytes': p.stat().st_size, 'mtime_ns': p.stat().st_mtime_ns}
              for p in sorted(DATA.glob('*/*.parquet'))],
    'query_sha256': {str(q): hashlib.sha256((OUT / 'workloads' / f'q{q}' / 'query.sql').read_bytes()).hexdigest()
                     for q in QUERIES},
    'config_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in [OUT / 'config.yaml', OUT / 'prefetch8.yaml']},
    'sequence': ['siblings_first', 'own_first', 'own_first', 'siblings_first'],
    'fresh_count': 3,
    'retained_count': 3,
}
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(OUT / 'dirty.patch').write_bytes(subprocess.check_output(['git', 'diff', 'HEAD'], cwd=ROOT))
print(json.dumps({'output': str(OUT), 'files': len(manifest['files']), 'bytes': sum(x['bytes'] for x in manifest['files'])}))
