import pathlib,subprocess,sys,json,datetime
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';PIXI=['pixi','run','--as-is','--manifest-path','/home/ubuntu/sirius/pixi.toml'];completed=[]
assert json.loads((E/'build-validation-completion.json').read_text())['status']=='complete'
assert json.loads((E/'harness-tests.command.json').read_text())['exit_status']==0
for plan in json.loads((E/'planned-campaigns.json').read_text())['campaigns']:
 name=plan['name'];cmd=plan['argv'];p=subprocess.run([sys.executable,'/tmp/s4f_capture.py','campaign-'+name,*cmd],cwd=ROOT);status={'name':name,'exit_status':p.returncode};completed.append(status);(E/'campaign-progress.json').write_text(json.dumps(completed,indent=2)+'\n')
 directory=cmd[cmd.index('--output')+1]
 analyzer=[*PIXI,'python','scripts/analyze_s4_experiments.py','--results',directory,'--output',str(E/(name+'-analysis.json'))]
 a=subprocess.run([sys.executable,'/tmp/s4f_capture.py','analyze-'+name,*analyzer],cwd=ROOT);status['analyzer_exit_status']=a.returncode;(E/'campaign-progress.json').write_text(json.dumps(completed,indent=2)+'\n')
 if p.returncode or a.returncode:
  print('Campaign stopped; failed directory preserved:',directory,flush=True);sys.exit(1)
(E/'campaign-completion.json').write_text(json.dumps({'status':'complete','campaigns':completed,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2)+'\n');print('All four instructed campaigns and analyses completed.',flush=True)
