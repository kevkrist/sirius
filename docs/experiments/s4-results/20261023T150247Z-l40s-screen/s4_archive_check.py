import json,pathlib,tarfile,hashlib,subprocess,sys,collections
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); e=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'; target=pathlib.Path('/tmp/s4-audit-20261023T1622Z');target.mkdir(); archive=e/'screen-campaign.tar.gz'; checked=[]
with tarfile.open(archive) as tar:
 for member in tar.getmembers():
  assert not pathlib.Path(member.name).is_absolute() and '..' not in pathlib.Path(member.name).parts
  assert not member.issym() and not member.islnk(),member.name
  if member.isfile():
   data=tar.extractfile(member).read(); original=root/member.name; assert hashlib.sha256(data).digest()==hashlib.sha256(original.read_bytes()).digest(),member.name
   assert not member.name.endswith(('.so','.a','.duckdb_extension','.parquet')) and pathlib.Path(member.name).name not in ('duckdb','sirius_unittest','sirius_cuda_fault_test','sirius_s4_experiment'),member.name
   checked.append(member.name)
 tar.extractall(target,filter='data')
assert sum(n.endswith('/native.jsonl') for n in checked)==86
p=subprocess.run([sys.executable,str(root/'scripts/analyze_s4_experiments.py'),'--results',str(target/'build/s4-results/20261023T150247Z-l40s-screen'),'--output',str(e/'relocated-screen-analysis.json')],check=True)
a=json.loads((e/'screen-analysis.json').read_text())['campaigns'][0]; b=json.loads((e/'relocated-screen-analysis.json').read_text())['campaigns'][0]
for c in (a,b): c.pop('directory'); c['stage_a'].pop('directory')
assert a==b
out={'status':'passed','files_compared_byte_for_byte':len(checked),'native_jsonl_files':86,'archive_sha256':hashlib.file_digest(archive.open('rb'),'sha256').hexdigest(),'relocated_root':str(target),'relocated_analyzer_exit_status':p.returncode,'relocated_analysis_matches':'all fields except campaign/stage-A directory paths'}
(e/'archive-verification.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
