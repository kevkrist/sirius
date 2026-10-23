import pathlib,json,hashlib,collections,sys
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';plans=json.loads((E/'planned-campaigns.json').read_text())['campaigns'];audits=[]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for plan in plans:
 directory=ROOT/plan['argv'][plan['argv'].index('--output')+1];run=json.loads((directory/'experiment.json').read_text());completion=json.loads((directory/'completion.json').read_text());assert completion['status']=='complete';assert sha(directory/'experiment.json')==completion['experiment_sha256'];assert sha(directory/'samples.jsonl')==completion['samples_sha256'];rows=[json.loads(l) for l in (directory/'samples.jsonl').read_text().splitlines() if l];groups=collections.defaultdict(list)
 for r in rows:groups[r['harness']['process_id']].append(r)
 assert len(groups)==len(run['jobs'])==completion['processes'];rawfiles=[]
 for job in run['jobs']:
  pid=job['process_id'];p=directory/'processes'/pid/'native.jsonl';native=[json.loads(l) for l in p.read_text().splitlines() if l];assert native==[{k:v for k,v in r.items() if k!='harness'} for r in groups[pid]],pid;rawfiles.append({'path':str(p.relative_to(directory)),'sha256':sha(p),'records':len(native)})
 harness=[]
 for name,item in run['harness_sources'].items():
  copied=E/'measured-harness'/pathlib.Path(item['path']).name;assert sha(copied)==item['sha256'];harness.append({'name':name,'sha256':item['sha256'],'returned_path':str(copied.relative_to(E))})
 sql=[r for r in rows if r['mode']=='sql'];oracle={k:sorted({r.get(k) for r in sql},key=str) for k in ('oracle_policy','oracle_rel_tol','oracle_abs_tol','gpu_intercepted','profile_verified','oracle_passed','fallbacks','runtime_fallbacks','read_view_mismatches')};statuses=dict(collections.Counter(r['status'] for r in rows));assert set(statuses)=={'ok'};assert all(r['oracle_passed'] for r in rows)
 audit={'campaign':plan['name'],'status':'passed','directory':str(directory),'processes':len(run['jobs']),'records':len(rows),'warmup_records':sum(r['warmup'] for r in rows),'measured_records':sum(not r['warmup'] for r in rows),'statuses':statuses,'sql_oracle_and_interception':oracle,'harness_sources':harness,'native_files':rawfiles};audits.append(audit);(E/(plan['name']+'-audit.json')).write_text(json.dumps(audit,indent=2)+'\n')
(E/'campaign-audit-summary.json').write_text(json.dumps({'status':'passed','campaigns':[{k:v for k,v in a.items() if k not in ('native_files','harness_sources')} for a in audits]},indent=2)+'\n');print(json.dumps({'campaigns':len(audits),'processes':sum(a['processes'] for a in audits),'records':sum(a['records'] for a in audits)},indent=2))
