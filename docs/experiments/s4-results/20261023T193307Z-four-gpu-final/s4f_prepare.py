import pathlib,subprocess,shutil,json,hashlib,datetime
repo=pathlib.Path('/home/ubuntu/sirius');old=pathlib.Path('/tmp/s4-four-gpu-20261023');final=repo/'build/s4-final-validation';snapshot='abe5dfe7';branch='experiment/s4-final-validation-20261023';evidence_rel=pathlib.Path('docs/experiments/s4-results/20261023T193307Z-four-gpu-final')
def run(args,cwd=repo):
 print('+ '+' '.join(map(str,args)),flush=True);return subprocess.run(list(map(str,args)),cwd=cwd,check=True)
initial=subprocess.check_output(['git','status','--short'],cwd=repo,text=True)
backup=pathlib.Path('/tmp/s4-prior-local-copy-20261023T193307Z');backup.mkdir()
for rel in [pathlib.Path('S4_EXPERIMENT_RESULTS.md'),pathlib.Path('docs/experiments/s4-results/20261023T150247Z-l40s-screen')]:
 src=repo/rel
 tracked=subprocess.check_output(['git','ls-tree','-r','--name-only','origin/'+branch,'--',str(rel)],cwd=repo,text=True).splitlines()
 for name in tracked:
  p=repo/name
  assert p.read_bytes()==subprocess.check_output(['git','show','origin/'+branch+':'+name],cwd=repo),name
 if src.exists():shutil.move(str(src),str(backup/src.name))
run(['git','switch','--track','-c',branch,'origin/'+branch]);run(['git','pull','--ff-only','--no-recurse-submodules','origin',branch])
run(['git','worktree','add','--detach',final,'origin/'+branch])
refs=[(final/'build/reference-prerequisite','78a3e34c66143152ff075729f4c018806c11e67d'),(final/'build/reference-control','9b2e8201787f381b62715c01405f659b925c49a7')]
for path,sha in refs:run(['git','worktree','add','--detach',path,sha])
e=final/evidence_rel;e.mkdir(parents=True);(e/'preparation.json').write_text(json.dumps({'start_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'original_status':initial,'original_status_after_update':subprocess.check_output(['git','status','--short'],cwd=repo,text=True),'final_source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=final,text=True).strip(),'final_initial_status':subprocess.check_output(['git','status','--porcelain'],cwd=final,text=True),'historical_copy_backup':str(backup),'references':[{'path':str(p),'commit':sha} for p,sha in refs]},indent=2)+'\n')
# Preserve reusable, version-pinned fetched sources before reclaiming this task's old outputs.
shared=repo/'build/s4-final-fetch-sources';shared.mkdir()
for name in ('cuco','corrosion'):
 source=old/'build/release/_deps'/f'{name}-src';dest=shared/f'{name}-src';shutil.move(str(source),str(dest))
# The prior raw campaign, source, committed logs/caches and hashes remain intact.
subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=old/'docs/experiments/s4-results/20261023T150247Z-l40s-screen',check=True,stdout=subprocess.DEVNULL)
removed=[]
for path in [old/'build/release',old/'build/s4-retry-xxhash',old/'sirius-duckdb/build']:
 assert path.is_relative_to(old) and path.is_dir();removed.append(str(path));shutil.rmtree(path)
(e/'reclaimed-own-builds.json').write_text(json.dumps({'removed_prior_generated_build_directories':removed,'preserved':'historical source and complete raw campaign; all committed evidence and artifact hashes','shared_fetch_sources':str(shared)},indent=2)+'\n')
print('Final root:',final,'Evidence:',e,flush=True)
