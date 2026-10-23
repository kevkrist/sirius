import pathlib,json,shutil,hashlib,tarfile,datetime,subprocess
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); rel=pathlib.Path('docs/experiments/s4-results/20261023T150247Z-l40s-screen'); e=root/rel; build=root/'build/s4-retry-xxhash'; campaign=root/'build/s4-results/20261023T150247Z-l40s-screen'; records=e/'build-records'; records.mkdir(exist_ok=True)
for n in ('build-manifest.json','builds.json'):
 if (build/n).exists(): shutil.copy2(build/n,records/n)
validation=[]
for schedule in ('current_direct','concurrent_ingress'):
 dest=records/schedule; dest.mkdir(exist_ok=True)
 for n in ('manifest.json','CMakeCache.txt','build-command.json'):
  if (build/schedule/n).exists(): shutil.copy2(build/schedule/n,dest/n)
 for n in ('sirius_unittest','sirius_cuda_fault_test'):
  p=build/schedule/n
  if p.exists(): validation.append({'schedule':schedule,'path':str(p),'size':p.stat().st_size,'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest()})
for src,name in [(root/'build/release/CMakeCache.txt','native-CMakeCache-after-both.txt'),(root/'sirius-duckdb/build/release/CMakeCache.txt','wrapper-CMakeCache-after-both.txt'),(root/'build/release/CMakeFiles/CMakeConfigureLog.yaml','native-CMakeConfigureLog.yaml')]:
 if src.exists(): shutil.copy2(src,records/name)
(e/'validation-artifact-hashes.json').write_text(json.dumps(validation,indent=2)+'\n')
for p in pathlib.Path('/tmp').glob('s4_*.py'):
 if p.name in ('s4_capture.py','s4_preflight.py','s4_tests.py','s4_verify.py','s4_campaign.py','s4_package.py','s4_report.py','s4_summaries.py','s4_report_tables.py'): shutil.copy2(p,e/p.name)
archives=[]
for source,name in [(campaign,'screen-campaign.tar.gz'),(root/'build/s4','build-attempt1-incomplete.tar.gz'),(root/'build/s4-retry-xxhsum','build-attempt2-incomplete.tar.gz')]:
 if source.exists():
  dest=e/name
  with tarfile.open(dest,'w:gz',compresslevel=6) as tar: tar.add(source,arcname=str(source.relative_to(root)))
  archives.append({'originating_host_path':str(source),'archive':name,'archive_prefix':str(source.relative_to(root)),'size':dest.stat().st_size,'sha256':hashlib.file_digest(dest.open('rb'),'sha256').hexdigest()})
(e/'archive-map.json').write_text(json.dumps(archives,indent=2)+'\n')
state={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'tracked_changes':subprocess.check_output(['git','status','--porcelain=v1','--untracked-files=no'],cwd=root,text=True),'recursive_submodules':subprocess.check_output(['git','submodule','status','--recursive'],cwd=root,text=True)}
(e/'ending-source.json').write_text(json.dumps(state,indent=2)+'\n')
print(json.dumps({'archives':archives,'tracked_source_clean':state['tracked_changes']==''},indent=2))
