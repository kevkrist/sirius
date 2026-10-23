import pathlib,json,tarfile,hashlib,subprocess,sys,itertools,io
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';DEST=pathlib.Path('/tmp/s4-final-archive-audit-20261023T193307Z');DEST.mkdir();mapping=json.loads((E/'archive-map.json').read_text());inventories={a['campaign']:{f['path']:f for f in a['files']} for a in json.loads((E/'archive-files.json').read_text())};audits=[]
for entry in mapping:
 parts=[E/item['file'] for item in entry['archive_parts']]
 for p,item in zip(parts,entry['archive_parts']):assert hashlib.sha256(p.read_bytes()).hexdigest()==item['sha256']
 # Campaign archives are small enough to reconstruct for this bounded offline audit.
 stream=io.BytesIO(b''.join(p.read_bytes() for p in parts));expected=inventories[entry['campaign']];found={}
 with tarfile.open(fileobj=stream,mode='r:gz') as tar:
  for member in tar.getmembers():
   p=pathlib.Path(member.name);assert not p.is_absolute() and '..' not in p.parts;assert not member.issym() and not member.islnk()
   if member.isfile():
    data=tar.extractfile(member).read();digest=hashlib.sha256(data).hexdigest();assert member.name in expected and digest==expected[member.name]['sha256'];found[member.name]=digest
    assert p.suffix not in ('.parquet','.so','.a','.duckdb_extension') and p.name not in ('duckdb','sirius_unittest','sirius_cuda_fault_test','sirius_s4_experiment')
  assert set(found)==set(expected);tar.extractall(DEST,filter='data')
 name=entry['campaign'];output=E/(name+'-relocated-analysis.json');log=E/(name+'-relocated-analysis.log');cmd=[sys.executable,str(E/'measured-harness/analyze_s4_experiments.py'),'--results',str(DEST/entry['archive_prefix']),'--output',str(output)]
 with log.open('w') as f:p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
 record={'campaign':name,'archive_files_verified':len(found),'native_jsonl_files':sum(n.endswith('/native.jsonl') for n in found),'relocated_analyzer_argv':cmd,'relocated_analyzer_exit_status':p.returncode}
 if entry['campaign_status']=='complete':
  assert p.returncode==0;old=json.loads((E/(name+'-analysis.json')).read_text());new=json.loads(output.read_text())
  for obj in (old,new):
   for c in obj['campaigns']:c.pop('directory',None);c['stage_a'].pop('directory',None)
  assert old==new,name;record['analysis_matches_except_directory_paths']=True
 else:record['analysis_unavailable']='campaign incomplete; rejection retained'
 audits.append(record)
(E/'archive-audit.json').write_text(json.dumps({'status':'passed','extraction_root':str(DEST),'campaigns':audits},indent=2)+'\n');print(json.dumps({'campaigns':len(audits),'files_verified':sum(a['archive_files_verified'] for a in audits)},indent=2))
