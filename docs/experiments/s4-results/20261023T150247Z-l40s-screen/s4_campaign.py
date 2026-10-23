import json,pathlib,subprocess,sys,os,importlib.util
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); evidence=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'; os.environ['S4_EVIDENCE_DIR']=str(evidence)
plan=json.loads((evidence/'planned-screen.json').read_text())
commands=[('screen',plan['argv']),('analyze-screen',['pixi','run','--as-is','--manifest-path','/home/ubuntu/sirius/pixi.toml','python','scripts/analyze_s4_experiments.py','--results','build/s4-results/20261023T150247Z-l40s-screen','--output',str(evidence/'screen-analysis.json')])]
results=[]
for label,cmd in commands:
 p=subprocess.run([sys.executable,'/tmp/s4_capture.py',label,*cmd],cwd=root); results.append({'step':label,'exit_status':p.returncode})
if all(r['exit_status']==0 for r in results):
 p=subprocess.run([sys.executable,'/tmp/s4_capture.py','verify-screen','pixi','run','--as-is','--manifest-path','/home/ubuntu/sirius/pixi.toml','python','/tmp/s4_verify.py'],cwd=root); results.append({'step':'verify-screen','exit_status':p.returncode})
(evidence/'campaign-status.json').write_text(json.dumps(results,indent=2)+'\n')
sys.exit(0 if all(r['exit_status']==0 for r in results) else 1)
