import pathlib,subprocess,sys,json,shutil
final=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');old=pathlib.Path('/tmp/s4-four-gpu-20261023');e=final/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';roots=[final,final/'build/reference-prerequisite',final/'build/reference-control'];paths=['duckdb','cucascade','substrait','.claude/claude-tools','sirius-duckdb/vcpkg','sirius-duckdb/duckdb','sirius-duckdb/extension-ci-tools','substrait/duckdb','substrait/extension-ci-tools','substrait/substrait'];records=[]
for root in roots:
 for rel in paths:
  p=pathlib.Path(rel);parent=root/'substrait' if rel.startswith('substrait/') else root;name=p.name if rel.startswith('substrait/') else rel
  line=subprocess.check_output(['git','ls-tree','HEAD','--',name],cwd=parent,text=True).split();assert line[0]=='160000',(parent,name,line);sha=line[2];target=root/p
  subprocess.run(['git','clone','--quiet','--shared','--no-checkout',str(old/p),str(target)],check=True)
  subprocess.run(['git','checkout','--quiet','--detach',sha],cwd=target,check=True)
 subprocess.run(['git','submodule','update','--init','--recursive','--',*paths[:7]],cwd=root,check=True)
 status=subprocess.check_output(['git','submodule','status','--recursive'],cwd=root,text=True);assert all(not l.startswith(('+','U')) for l in status.splitlines());records.append({'root':str(root),'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'recursive_submodules':status,'before_overlay_status':subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=root,text=True)})
sys.path.insert(0,str(final/'scripts'));from run_s4_experiments import BASELINE_OVERLAY,baseline_cmake,source_identity
for root,kind in zip(roots[1:],('prerequisite','s4_control')):
 for name in BASELINE_OVERLAY:
  dest=root/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(final/name,dest)
 original=subprocess.check_output(['git','show','HEAD:CMakeLists.txt'],cwd=root,text=True);(root/'CMakeLists.txt').write_text(baseline_cmake(original));records.append({'role':'baseline','kind':kind,'source_identity':source_identity(root,'baseline',kind)})
records.append({'role':'final','source_identity':source_identity(final,'final')});(e/'source-and-overlay-identities.json').write_text(json.dumps(records,indent=2)+'\n');print('All three pinned dependency trees and allowlisted overlays verified.',flush=True)
