#!/usr/bin/env python3
"""qdf_sum.py <quent_df json> [...]: per (query, join) over hot iterations (iter>=2): Publishing Task ms, publication @ms,
first consumer scan created/started minus publication (min over iterations), scan tasks started before publication (sum)."""
import json,sys,collections,statistics as st
rows=collections.defaultdict(list)
for f in sys.argv[1:]:
    for rec in json.load(open(f)):
        if rec.get('iter',0)<2: continue
        for j in rec['joins']:
            if 'tpub' not in j: continue
            rows[(rec['q'],j['join'])].append((rec,j))
print("| q | join | site | kinds | n | Publishing Task ms med | site union ms med | pub @ms med | site start -> first primary scan created ms med | consumer scans: min(created - pub) ms / min(first start - pub) ms / tasks started before pub (sum over iters) |")
print("|---|---|---|---|---|---|---|---|---|---|")
tot_before=0; n_inst=0
for (q,J),v in sorted(rows.items()):
    js=[j for _,j in v]; n_inst+=len(js)
    cons=collections.defaultdict(list)
    for j in js:
        for c in j.get('consumers',[]): cons[c['scan']].append(c)
    cs=[]
    for scan,cl in cons.items():
        b=sum(c['tasks_started_before_pub'] for c in cl); tot_before+=b
        cs.append(f"{scan.split(' ')[0]}{'' if cl[0]['primary'] else '*'}: {min(c['gap_pub_to_created'] for c in cl):+.1f} / {min(c['gap_pub_to_first'] for c in cl):+.1f} / {b}")
    spans=[]
    for j in js:
        pc=[c for c in j.get('consumers',[]) if c['primary']]
        if pc: spans.append(j['tpub']-j['first_c0']+min(c['gap_pub_to_created'] for c in pc))
    span=f"{st.median(spans):.1f}" if spans else "-"
    print(f"| {q} | {J} | {js[0]['site']} | {js[0]['kinds']} | {len(js)} | {st.median(j['comp_last'] for j in js):.1f} | {st.median(j['build_union'] for j in js):.1f} | {st.median(j['tpub'] for j in js):.0f} | {span} | {'; '.join(cs)} |")
print(f"\njoin-instances {n_inst}, consumer scan tasks started before publication: {tot_before}")
