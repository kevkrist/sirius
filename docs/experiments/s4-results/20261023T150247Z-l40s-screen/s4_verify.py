import pathlib,json,hashlib,collections,sys
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); e=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'; campaign=root/'build/s4-results/20261023T150247Z-l40s-screen'; run=json.loads((campaign/'experiment.json').read_text()); done=json.loads((campaign/'completion.json').read_text()); records=[json.loads(l) for l in (campaign/'samples.jsonl').read_text().splitlines() if l.strip()]
hashfile=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert done['status']=='complete'
assert done['experiment_sha256']==hashfile(campaign/'experiment.json')
assert done['samples_sha256']==hashfile(campaign/'samples.jsonl')
groups=collections.defaultdict(list)
for r in records: groups[r['harness']['process_id']].append(r)
checks=[]
for job in run['jobs']:
 pid=job['process_id']; saved=groups[pid]; native=list(campaign.rglob('native.jsonl')) if False else []
 matches=list(campaign.glob('**/'+pid+'/native.jsonl'))
 if not matches:
  request_matches=[p for p in campaign.rglob('native.jsonl') if p.parent.name==pid or pid in p.parent.name]
  matches=request_matches
 if len(matches)!=1: raise RuntimeError((pid,[str(p) for p in matches]))
 raw=[json.loads(l) for l in matches[0].read_text().splitlines() if l.strip()]
 aggregate=[{k:v for k,v in r.items() if k!='harness'} for r in saved]
 assert aggregate==raw,(pid,'aggregate/native mismatch')
 checks.append({'process_id':pid,'native':str(matches[0].relative_to(campaign)),'records':len(raw),'sha256':hashfile(matches[0])})
out={'status':'passed','processes':len(checks),'records':len(records),'warmups':sum(r['warmup'] for r in records),'measured':sum(not r['warmup'] for r in records),'statuses':dict(collections.Counter(r['status'] for r in records)),'oracle_outcomes':dict(collections.Counter(r['oracle_outcome'] for r in records)),'counts_by_mode':dict(collections.Counter(r['mode'] for r in records)),'checks':checks}
(e/'independent-bundle-verification.json').write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps({k:v for k,v in out.items() if k!='checks'},indent=2))
