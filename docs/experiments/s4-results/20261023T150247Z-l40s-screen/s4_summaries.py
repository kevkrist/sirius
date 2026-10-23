import pathlib,json,collections,statistics
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); e=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'; p=root/'build/s4-results/20261023T150247Z-l40s-screen'; run=json.loads((p/'experiment.json').read_text()); rows=[json.loads(l) for l in (p/'samples.jsonl').read_text().splitlines() if l]; measured=[r for r in rows if not r['warmup']]; groups=collections.defaultdict(list)
for r in measured: groups[(r['harness']['workload'],r['harness']['comparison'],r['harness']['arm'])].append(r)
summary=[]
for (workload,comparison,arm),records in sorted(groups.items()):
 stable={}
 for key in records[0]:
  if key in ('harness','sample_id','seed','order','repetition','process_cluster','pair','run_id','build_id','warmup','local_event_durations'): continue
  values={json.dumps(r.get(key),sort_keys=True) for r in records}
  stable[key]=[json.loads(v) for v in sorted(values)]
 summary.append({'workload':workload,'comparison':comparison,'arm':arm,'records':len(records),'sample_ids':[r['sample_id'] for r in records],'fields':stable,'host_elapsed_ns':[r.get('host_elapsed_ns') for r in records],'query_elapsed_ns':[r.get('query_elapsed_ns') for r in records]})
(e/'workload-summary.json').write_text(json.dumps({'note':'Descriptive summaries only; warmups excluded; null values retain unavailable measurements','groups':summary},indent=2)+'\n')
workloads={w['id']:w for w in run['workloads']}
lines=['| Workload | Pattern / ordered edges | Root | Issuer | Logical bytes / repetition | Host elapsed ns (3 repetitions) | Median logical GB/s |','| --- | --- | ---: | --- | ---: | --- | ---: |']
for w in run['workloads']:
 if w['mode']!='transport': continue
 rs=groups[(w['id'],'diagnostic','baseline')]; good=[r for r in rs if r['status']=='ok']; edges='; '.join(f'{a}→{b}' for a,b in zip(rs[0].get('edge_sources',[]),rs[0].get('edge_destinations',[]))) if rs else 'unavailable'
 times=[r.get('host_elapsed_ns') for r in good]; rates=[r['logical_edge_bytes']/r['host_elapsed_ns'] for r in good]; bytevalues=sorted({r.get('logical_edge_bytes') for r in good})
 lines.append('| '+ ' | '.join([w['id'],w['pattern']+': '+edges,str(w['root']),w['issuer'],str(bytevalues),', '.join(map(str,times)),f'{statistics.median(rates):.3f}' if rates else 'unavailable'])+' |')
(e/'stage-a-table.md').write_text('\n'.join(lines)+'\n')
print(json.dumps({'measured_records':len(measured),'groups':len(summary),'statuses':dict(collections.Counter(r['status'] for r in measured))},indent=2))
