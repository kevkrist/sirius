import pathlib,json,hashlib,tarfile,shutil,subprocess,datetime
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';plans=json.loads((E/'planned-campaigns.json').read_text())['campaigns'];mapping=[];inventory=[]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for plan in plans:
 directory=ROOT/plan['argv'][plan['argv'].index('--output')+1]
 if not directory.exists(): continue
 prefix=str(directory.relative_to(ROOT));path=E/(plan['name']+'.tar.gz');files=[]
 for f in sorted(directory.rglob('*')):
  if f.is_file():
   assert not f.is_symlink(),str(f)
   files.append({'path':str(f.relative_to(ROOT)),'size':f.stat().st_size,'sha256':sha(f)})
 with tarfile.open(path,'w:gz',compresslevel=6) as tar:tar.add(directory,arcname=prefix)
 parts=[]
 if path.stat().st_size>75*1024**2:
  with path.open('rb') as source:
   index=0
   while data:=source.read(64*1024**2):
    part=path.with_name(path.name+f'.part{index:03d}');part.write_bytes(data);parts.append({'file':part.name,'sha256':sha(part),'size':part.stat().st_size});index+=1
  path.unlink()
 else:parts=[{'file':path.name,'sha256':sha(path),'size':path.stat().st_size}]
 mapping.append({'campaign':plan['name'],'originating_host_path':str(directory),'archive_prefix':prefix,'archive_parts':parts,'compression':'gzip tar; concatenate ordered parts if split','campaign_status':json.loads((directory/'completion.json').read_text())['status'] if (directory/'completion.json').exists() else 'failed/incomplete','file_count':len(files)})
 inventory.append({'campaign':plan['name'],'files':files})
(E/'archive-map.json').write_text(json.dumps(mapping,indent=2)+'\n');(E/'archive-files.json').write_text(json.dumps(inventory,indent=2)+'\n')
for name in ['s4f_prepare.py','s4f_capture.py','s4f_dependencies.py','s4f_preflight.py','s4f_build_validate.py','s4f_run_campaigns.py','s4f_audit.py','s4f_package.py','s4f_archive_audit.py','s4f_tables.py','s4f_report.py']:
 source=pathlib.Path('/tmp')/name
 if source.exists():shutil.copy2(source,E/name)
for name in ['test/cpp/operator/s4_experiment.cpp','test/cpp/operator/s4_experiment_common.hpp','test/cpp/operator/s4_experiment_sql.cpp','test/cpp/operator/s4_experiment_sql_oracle.hpp','cmake/sirius-s4-baseline-fixture.cmake']:
 dest=E/'measured-fixture'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,dest)
source=[]
for path,role,kind in [(ROOT,'final','prerequisite'),(ROOT/'build/reference-prerequisite','baseline','prerequisite'),(ROOT/'build/reference-control','baseline','s4_control')]:
 source.append({'path':str(path),'role':role,'baseline_kind':kind if role=='baseline' else None,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=path,text=True).strip(),'status':subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=path,text=True),'submodules':subprocess.check_output(['git','submodule','status','--recursive'],cwd=path,text=True)})
(E/'ending-source-state.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sources':source},indent=2)+'\n');print(json.dumps({'campaign_archives':mapping},indent=2))
