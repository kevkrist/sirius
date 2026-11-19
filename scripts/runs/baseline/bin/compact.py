import json,sys
s=json.load(open('logpass/df_summary.json'))
mb=lambda b: f"{b/2**20:.1f}"
print("| q | join | type | layout/path | key kinds | valid rows | domain (prov) | bloom MB | #batches | GPUs leased | bloom src/tgt | pub ms after final | HOST-staged | build PARTITION sum/max ms | note |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for q in sorted(s,key=int):
    for j in s[q]['joins']:
        if not j['keys']: continue
        path = 'hash(scatter)' if j['layout'].endswith('hash') and not j['layout'].startswith('1p') else 'forward'
        kinds='+'.join(k['kind'] for k in j['keys'])
        valid=j['keys'][0].get('valid')
        dom=';'.join(f"{d['domain']}({d['provenance'].replace('pinned_parquet_','')})" if d['domain'] else 'unknown' for d in j['doms']) or '?'
        bb=sum(int(k['size']) for k in j['keys'] if k['kind']=='bloom')
        st=j['started'][0] if j['started'] else None
        fin=j['fins'][0] if j['fins'] else None
        pub=j['pubs'][0] if j['pubs'] else None
        bp=j.get('build_partition') or []
        bp=[b for b in bp if st and b['tasks']==st['batches']] or bp
        bps='; '.join(f"{b['sum_ms']}/{b['max_ms']}" for b in bp[:2]) if bp else '?'
        note=';'.join(sorted(set(k['reason'] for k in j['keys'] if k['reason']!='-')))
        pushed='/'.join(k['pushed'] for k in j['keys'])
        print(f"| {q} | {j['join']} | {j.get('jtype')} | {j['layout']}{' bc' if j['broadcast'] else ''} / {path} | {kinds} | {valid} | {dom} | {mb(bb) if bb else '-'} | {st['batches'] if st else '-'} | {st['gpus'] if st else '-'} | {str(fin['sources'])+'/'+str(fin['targets']) if fin else '-'} | {pub['ms'] if pub else '-'} | {('yes' if pub['host'] else 'no') if pub else '-'} | {bps} | pushed={pushed} {note} |")
