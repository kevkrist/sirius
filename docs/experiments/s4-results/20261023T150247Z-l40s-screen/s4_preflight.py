import importlib.util,json,pathlib,sys,subprocess,hashlib
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); evidence=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'
spec=importlib.util.spec_from_file_location('s4runner',root/'scripts/run_s4_experiments.py'); m=importlib.util.module_from_spec(spec); sys.modules[spec.name]=m; spec.loader.exec_module(m)
(evidence/'machine-preflight.json').write_text(json.dumps(m.machine_info(),indent=2)+'\n')
import pyarrow.parquet as pq
base=pathlib.Path('/data/tpch_parquet_sf100'); md=json.loads((base/'metadata.json').read_text()); files=[]; counts={}
for table in md['tables']:
    counts[table]=0
    for f in sorted((base/table).rglob('*.parquet')):
        stat=f.stat(); pf=pq.ParquetFile(f); counts[table]+=pf.metadata.num_rows
        files.append({'path':str(f),'size':stat.st_size,'mtime_ns':stat.st_mtime_ns,'rows':pf.metadata.num_rows,'row_groups':pf.metadata.num_row_groups})
record={'path':str(base),'scale':md['options']['scale_factor'],'scale_basis':'generator metadata and independently summed Parquet footer row counts','metadata_sha256':hashlib.sha256((base/'metadata.json').read_bytes()).hexdigest(),'row_counts':counts,'declared_row_counts':{k:v['row_count'] for k,v in md['tables'].items()},'files':files,'content_hashes':'not measured; footer metadata only'}
(evidence/'dataset-identity.json').write_text(json.dumps(record,indent=2)+'\n'); (evidence/'dataset-generator-metadata.json').write_bytes((base/'metadata.json').read_bytes()); print(json.dumps({k:v for k,v in record.items() if k!='files'},indent=2))
