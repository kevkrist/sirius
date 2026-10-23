import pathlib,subprocess,sys,shutil,json,datetime,os,hashlib
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';PIXI=['pixi','run','--as-is','--manifest-path','/home/ubuntu/sirius/pixi.toml'];BASE=ROOT/'build/reference-prerequisite';CONTROL=ROOT/'build/reference-control';shared=pathlib.Path('/home/ubuntu/sirius/build/s4-final-fetch-sources');opts=['--cmake-option=-DSIRIUS_TPCH_HAVOC=OFF',f'--cmake-option=-DFETCHCONTENT_SOURCE_DIR_CUCO={shared}/cuco-src',f'--cmake-option=-DFETCHCONTENT_SOURCE_DIR_CORROSION={shared}/corrosion-src','--cmake-option=-DXXHSUM_EXECUTABLE=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/bin/xxhsum','--cmake-option=-DSIMPATICO_XXHASH_INCLUDE_DIR=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/include'];completed=[]
def capture(label,cmd):
 p=subprocess.run([sys.executable,'/tmp/s4f_capture.py',label,*map(str,cmd)],cwd=ROOT);completed.append({'step':label,'exit_status':p.returncode});(E/'build-validation-progress.json').write_text(json.dumps(completed,indent=2)+'\n')
 if p.returncode:sys.exit(p.returncode)
def freeze_records(source,dest,label):
 rec=E/'build-records'/label;rec.mkdir(parents=True)
 for n in ('manifest.json','CMakeCache.txt','build-command.json'):shutil.copy2(dest/n,rec/n)
 for path,name in [(source/'sirius-duckdb/build/release/CMakeCache.txt','wrapper-CMakeCache.txt'),(source/'build/release/CMakeFiles/CMakeConfigureLog.yaml','CMakeConfigureLog.yaml')]:
  if path.exists():shutil.copy2(path,rec/name)
 extra=[]
 for name in ('sirius_unittest','sirius_cuda_fault_test'):
  p=dest/name;extra.append({'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'size':p.stat().st_size})
 (rec/'test-artifact-hashes.json').write_text(json.dumps(extra,indent=2)+'\n')
def reclaim(source,label):
 paths=[source/'build/release',source/'sirius-duckdb/build'];assert all(p.is_relative_to(ROOT) for p in paths)
 for p in paths:shutil.rmtree(p)
 (E/f'{label}-reclaimed-intermediates.json').write_text(json.dumps({'source':str(source),'removed':[str(p) for p in paths],'reason':'disk capacity; frozen binaries/manifests, exact caches and source remain'},indent=2)+'\n')
selection='[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]'
for label,role,kind,source,output in [('prerequisite','baseline','prerequisite',BASE,ROOT/'build/s4-revisions/prerequisite-artifacts'),('final','final','prerequisite',ROOT,ROOT/'build/s4-revisions/final-artifacts'),('control','baseline','s4_control',CONTROL,ROOT/'build/s4-revisions/control-artifacts')]:
 capture('build-'+label,[*PIXI,'python','scripts/build_s4_experiments.py','--baseline-source',BASE if role=='final' else source,'--baseline-kind',kind,'--revision-role',role,'--output',output,'--cuda-architectures','native','--jobs','24',*opts])
 dest=output/role;freeze_records(source,dest,label)
 if role=='final':
  capture('final-focused',[*PIXI,dest/'sirius_unittest','[s4_publication]','--order','decl'])
  capture('final-sql-oracle',[*PIXI,dest/'sirius_unittest','[sql_oracle]','--order','decl'])
 else:capture(label+'-cumulative',[*PIXI,dest/'sirius_unittest',selection,'--order','decl'])
 capture(label+'-fault',[*PIXI,dest/'sirius_cuda_fault_test'])
 reclaim(source,label)
for label,baseline in [('prerequisite',ROOT/'build/s4-revisions/prerequisite-artifacts/baseline/manifest.json'),('control',ROOT/'build/s4-revisions/control-artifacts/baseline/manifest.json')]:
 pair=ROOT/f'build/s4-revisions/{label}/build-manifest.json';pair.parent.mkdir(parents=True,exist_ok=True)
 capture('pair-'+label,[*PIXI,'python','scripts/run_s4_experiments.py','pair-manifests','--baseline',baseline,'--final',ROOT/'build/s4-revisions/final-artifacts/final/manifest.json','--output',pair]);shutil.copy2(pair,E/'build-records'/f'{label}-build-manifest.json')
(E/'build-validation-completion.json').write_text(json.dumps({'status':'complete','steps':completed,'measured_final_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()},indent=2)+'\n');print('All three builds, applicable tests and both pair identities passed.',flush=True)
