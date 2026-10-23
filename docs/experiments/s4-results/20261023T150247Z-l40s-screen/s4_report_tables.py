import json,pathlib,statistics,collections
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); e=root/'docs/experiments/s4-results/20261023T150247Z-l40s-screen'; p=root/'build/s4-results/20261023T150247Z-l40s-screen'; run=json.loads((p/'experiment.json').read_text()); analysis=json.loads((e/'screen-analysis.json').read_text()); analysis=analysis[0] if isinstance(analysis,list) else analysis
if 'campaigns' in analysis: analysis=analysis['campaigns'][0]
rs=[json.loads(l) for l in (p/'samples.jsonl').read_text().splitlines() if l]; rs=[r for r in rs if not r['warmup']]; groups=collections.defaultdict(list)
for r in rs: groups[(r['harness']['workload'],r['harness']['comparison'],r['harness']['arm'])].append(r)
outcomes={(o['workload']['id'],o['comparison'],o['metric']):o for o in analysis['outcomes']}
def table(headers,rows): return '\n'.join(['| '+' | '.join(headers)+' |','| '+' | '.join('---' for h in headers)+' |']+['| '+' | '.join(str(v).replace('|','\\|').replace('\n',' ') for v in row)+' |' for row in rows])+'\n'
def unique(records,key): return ', '.join(sorted({json.dumps(r.get(key),sort_keys=True) for r in records}))
def ms(value): return f'{value/1e6:.6f}' if value is not None else 'unavailable'
def stat(o,field): return f"{o['statistics'][field]:.4f}" if o.get('statistics') else 'unavailable'
builders=[w for w in run['workloads'] if w['mode']=='builder']; geometry=[]; timings=[]; raw=[]
for w in builders:
 wid=w['id']; records=groups[(wid,'baseline_candidate','baseline')]+groups[(wid,'baseline_candidate','candidate')]; r=records[0]
 geometry.append([wid,w['root'],r.get('contributing_gpu_count'),w['K'],w['B'],unique(records,'C'),unique(records,'actual_transport'),unique(records,'input_bitmap_bytes'),unique(records,'ready_replica_bytes'),unique(records,'logical_edge_bytes'),unique(records,'scratch_requested_bytes')+'/'+unique(records,'scratch_aligned_bytes'),unique(records,'host_staging_physical_bytes'),unique(records,'d2h_bytes')+'/'+unique(records,'h2d_bytes')])
 o=outcomes[(wid,'baseline_candidate','host_elapsed_ns')]; c=outcomes[(wid,'baseline_baseline','host_elapsed_ns')]
 timings.append([wid,ms(o['baseline']['median_ns']) if o['baseline'] else 'unavailable',ms(o['candidate']['median_ns']) if o['candidate'] else 'unavailable',ms(o['baseline']['p90_ns']) if o['baseline'] else 'unavailable',ms(o['candidate']['p90_ns']) if o['candidate'] else 'unavailable',stat(o,'median_ratio'),stat(o,'p90_ratio'),stat(c,'median_ratio'),stat(c,'p90_ratio')])
 for comparison in ('baseline_candidate','baseline_baseline'):
  a=groups[(wid,comparison,'baseline')]; b=groups[(wid,comparison,'candidate')];raw.append([wid,comparison,', '.join(str(r['host_elapsed_ns']) for r in a),', '.join(str(r['host_elapsed_ns']) for r in b)])
text='All byte columns are logical quantities or checked-out workspace charges as labeled. Each contributor inserts 4,096 rows/key; four contributors and four replicas are used. Builder returns ready filters and has zero channel-published bytes. Timing is host publication preparation through completion.\n\n'+table(['Workload','Root','Contributors','K','B/key bytes','C bytes','Transport','Input bytes','Ready bytes','Logical edges bytes','Scratch requested/aligned bytes','Physical HOST bytes','D2H/H2D bytes'],geometry)+'\n'+table(['Workload','Control median ms','Candidate median ms','Control p90 ms','Candidate p90 ms','C/B median paired ratio','C/B p90 ratio','B/B median ratio','B/B p90 ratio'],timings)+'\nAll raw measured host intervals, in ns (warmups remain in the archive):\n\n'+table(['Workload','Comparison','Baseline ns','Other arm ns'],raw)
(e/'builder-tables.md').write_text(text)
queries=[w for w in run['workloads'] if w['mode']=='sql']; timings=[]; counts=[]; raw=[]
for w in queries:
 wid=w['id']
 for metric in ('query_elapsed_ns','host_elapsed_ns'):
  o=outcomes[(wid,'baseline_candidate',metric)]; c=outcomes[(wid,'baseline_baseline',metric)]
  timings.append([wid,metric,ms(o['baseline']['median_ns']) if o['baseline'] else 'unavailable',ms(o['candidate']['median_ns']) if o['candidate'] else 'unavailable',ms(o['baseline']['p90_ns']) if o['baseline'] else 'unavailable',ms(o['candidate']['p90_ns']) if o['candidate'] else 'unavailable',stat(o,'median_ratio'),stat(o,'p90_ratio'),stat(c,'median_ratio'),stat(c,'p90_ratio'),json.dumps(o['excluded_pairs'],sort_keys=True)])
 for comparison in ('baseline_candidate','baseline_baseline'):
  for arm in ('baseline','candidate'):
   group=groups[(wid,comparison,arm)];counts.append([wid,comparison+'/'+arm,unique(group,'publication_count'),unique(group,'published_bitmap_bytes'),unique(group,'accumulation_input_bitmap_bytes'),unique(group,'accumulation_ready_replica_bytes'),unique(group,'accumulation_admitted_keys'),unique(group,'accumulation_contributing_gpus'),unique(group,'publication_skip_reason'),unique(group,'gpu_intercepted')+'/'+unique(group,'profile_verified')+'/'+unique(group,'oracle_passed')]);raw.append([wid,comparison+'/'+arm,', '.join(str(r['query_elapsed_ns']) for r in group),', '.join(str(r['host_elapsed_ns']) for r in group)])
text='Query intervals and publication intervals are distinct. Publication timing, when available, is the sum of all finish-attempt preparation-through-completion intervals in a query; it is not necessarily query critical-path time. Publication comparisons require equivalent admitted and published work. SQL per-publication root/K/B/C are not exposed by the snapshot.\n\n'+table(['Query','Metric','Control median ms','Candidate median ms','Control p90 ms','Candidate p90 ms','C/B median paired ratio','C/B p90 ratio','B/B median ratio','B/B p90 ratio','Excluded C/B pairs'],timings)+'\nCounter values per measured query execution (sets show all distinct observations):\n\n'+table(['Query','Comparison/arm','Publication count','Published bytes','Input bytes','Ready bytes','Admitted keys','Contributing GPUs','Skip reason','Interception/profile/oracle'],counts)+'\nRaw measured query and publication intervals, in ns:\n\n'+table(['Query','Comparison/arm','Query ns','Publication ns'],raw)
(e/'sql-tables.md').write_text(text)
print(json.dumps({'builder_timings':timings if False else len(builders),'queries':len(queries),'analysis_keys':list(analysis)},indent=2))
