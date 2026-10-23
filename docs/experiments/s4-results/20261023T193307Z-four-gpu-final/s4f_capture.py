import datetime,json,pathlib,subprocess,sys,time,os
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';label=sys.argv[1];cmd=sys.argv[2:];start=datetime.datetime.now(datetime.timezone.utc).isoformat();t=time.monotonic();env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',S4_EVIDENCE_DIR=str(E),S4_RESULT_REMOTE='origin')
with (E/(label+'.log')).open('x') as log:
 print('+ '+repr(cmd),file=log,flush=True)
 try:p=subprocess.run(cmd,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT);code=p.returncode
 except Exception as exc:print(repr(exc),file=log);code=127
record={'label':label,'argv':cmd,'cwd':str(ROOT),'start_utc':start,'end_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'elapsed_seconds':time.monotonic()-t,'exit_status':code,'log':label+'.log'};(E/(label+'.command.json')).write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record),flush=True);sys.exit(code)
