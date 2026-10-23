import importlib.util,pathlib,sys,json,hashlib,subprocess,os
root=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');e=root/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';spec=importlib.util.spec_from_file_location('s4runner',root/'scripts/run_s4_experiments.py');m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m);(e/'machine-preflight.json').write_text(json.dumps(m.machine_info(),indent=2)+'\n')
import pyarrow.parquet as pq
base=pathlib.Path('/data/tpch_parquet_sf100');md=json.loads((base/'metadata.json').read_text());files=[];counts={}
for table,declared in md['tables'].items():
 counts[table]=0
 for f in sorted((base/table).rglob('*.parquet')):
  st=f.stat();pf=pq.ParquetFile(f);counts[table]+=pf.metadata.num_rows;files.append({'path':str(f),'size':st.st_size,'mtime_ns':st.st_mtime_ns,'rows':pf.metadata.num_rows,'row_groups':pf.metadata.num_row_groups})
 assert counts[table]==declared['row_count'],table
record={'path':str(base),'scale':md['options']['scale_factor'],'scale_basis':'generator metadata and independent Parquet footer row counts','row_counts':counts,'metadata_sha256':hashlib.sha256((base/'metadata.json').read_bytes()).hexdigest(),'files':files,'dataset_full_content_hashes':'not measured'};(e/'dataset-identity.json').write_text(json.dumps(record,indent=2)+'\n');(e/'dataset-generator-metadata.json').write_bytes((base/'metadata.json').read_bytes())
packages=[]
for p in sorted(pathlib.Path(os.environ['CONDA_PREFIX'],'conda-meta').glob('*.json')):
 d=json.loads(p.read_text());packages.append({k:d.get(k) for k in ('name','version','build','channel','sha256')})
(e/'installed-packages.json').write_text(json.dumps(packages,indent=2)+'\n');print(json.dumps({'dataset':str(base),'scale':record['scale'],'rows':counts,'files':len(files),'gpu_capture':m.machine_info()['observations']['gpus']},indent=2))
