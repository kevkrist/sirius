import json
from pathlib import Path

import duckdb

out = Path('/tmp/tpch-deferred-order-sf100-20261015')
data = Path('/data/tpch_parquet_sf100')
con = duckdb.connect()
rows = []
for table in sorted(data.iterdir()):
    if not table.is_dir():
        continue
    result = con.execute('SELECT file_name, num_rows, num_row_groups FROM parquet_file_metadata(?)', [str(table / '*.parquet')]).fetchall()
    for path, count, groups in result:
        rows.append({'table': table.name, 'path': path, 'rows': count, 'row_groups': groups})
(out / 'dataset_metadata.json').write_text(json.dumps({'duckdb_metadata_reader_version': duckdb.__version__, 'files': rows}, indent=2) + '\n')
print(json.dumps(rows, indent=2))
