import datetime,json,pathlib,subprocess,sys,time
root=pathlib.Path('/tmp/s4-four-gpu-20261023')
evidence=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'
label=sys.argv[1]; cmd=sys.argv[2:]
start=datetime.datetime.now(datetime.timezone.utc).isoformat(); t=time.monotonic()
with (evidence/(label+'.log')).open('w') as log:
    print('+ '+repr(cmd),file=log,flush=True)
    try:
        p=subprocess.run(cmd,cwd=root,stdout=log,stderr=subprocess.STDOUT)
        code=p.returncode
    except Exception as exc:
        print(repr(exc),file=log); code=127
record={'label':label,'argv':cmd,'cwd':str(root),'start_utc':start,'end_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'elapsed_seconds':time.monotonic()-t,'exit_status':code,'log':label+'.log'}
(evidence/(label+'.command.json')).write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record),flush=True)
sys.exit(code)
