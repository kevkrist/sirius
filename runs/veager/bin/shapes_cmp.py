#!/usr/bin/env python3
"""Verifier: compare shape results across arms. Usage: shapes_cmp.py <ref arm> <arm> ..."""
import sys, json, collections
V = "/opt/dlami/nvme/dfperf/runs/veager/shapes"
def load(a):
    d = collections.OrderedDict(); other = []
    try: lines = open(f"{V}/{a}/results.jsonl").read().splitlines()
    except FileNotFoundError: return None, None
    for l in lines:
        j = json.loads(l)
        if "iter" in j and not j["shape"].startswith("after_interrupt"): d.setdefault(j["shape"], []).append(j)
        else: other.append(j)
    return d, other
ref, arms = sys.argv[1], sys.argv[2:]
R, _ = load(ref)
print("| shape | " + ref + " s (hot) | " + " | ".join(f"{a}: result = {ref} / s (hot) / status" for a in arms) + " |")
print("|---|---|" + "---|" * len(arms))
D = {a: load(a) for a in arms}
for sh, recs in R.items():
    row = f"| {sh} | {recs[-1].get('s')} |"
    for a in arms:
        d = D[a][0]
        if d is None or sh not in d: row += " - |"; continue
        x = d[sh]
        same = all(r.get("md5") == recs[0].get("md5") for r in x) and all(r["status"] == "ok" for r in x)
        row += f" {'same' if same else 'DIFF ' + str(x[-1].get('head', x[-1].get('err')))[:80]} / {x[-1].get('s')} / {','.join(r['status'] for r in x)} |"
    print(row)
for a in arms:
    o = D[a][1]
    if o is None: continue
    c = collections.Counter(j["status"] for j in o if j["shape"].startswith("interrupt"))
    aft = [j for j in o if j["shape"].startswith("after_interrupt")]
    refmd5 = {sh: r[0].get("md5") for sh, r in R.items()}
    okaft = sum(1 for j in aft if j["status"] == "ok" and j.get("md5") == refmd5.get(j["shape"].replace("after_interrupt_", "").replace("ref", "selective")))
    hang = [j for j in o if j["status"] == "HANG"]
    print(f"{a}: interrupts {dict(c)}; queries after an interrupt correct {okaft}/{len(aft)} (s: {[j.get('s') for j in aft]}); HANG records {hang}; closed {'close' in [j['shape'] for j in o]}")
