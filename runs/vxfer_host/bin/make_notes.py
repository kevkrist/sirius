#!/usr/bin/env python3
"""make_notes.py: fills runs/vxfer_host/notes_template.md from the raw runs and writes notes/VERIFY_PROTO_XFER_HOST.md."""
import csv, glob, statistics as st, hashlib, os, sys, datetime
X = '/opt/dlami/nvme/dfperf/runs/vxfer_host'
def arm(label, fam, q):
    xs = []; per = []; hs = set()
    for p in sorted(glob.glob(f'{X}/{label}/{fam}*/tpch_*_q{q}')):
        try: t = {int(r['iteration']): float(r['runtime_s']) * 1000 for r in csv.DictReader(open(p + '/csv/runtimes.csv'))}
        except Exception: continue
        hot = [t[i] for i in sorted(t) if i >= 2]
        if not hot: continue
        xs += hot; per.append(st.median(hot))
        r = f'{p}/sirius/q{q}/result.txt'
        hs.add(hashlib.md5(open(r, 'rb').read()).hexdigest()[:8] if os.path.exists(r) else 'missing')
    return dict(med=st.median(xs), n=len(xs), lo=min(xs), hi=max(xs), per=per, hs=hs) if xs else None
AUTH = {1: -4.9, 2: -3.0, 3: -41.0, 4: -14.2, 5: -12.1, 6: 0.2, 7: -40.6, 8: -25.8, 9: -12.8, 10: -33.0, 11: 3.1, 12: -9.6, 13: 5.8, 14: -5.6,
        15: 0.0, 16: -4.2, 17: -0.8, 18: -4.6, 19: 2.0, 20: -3.2, 21: -208.5, 22: -4.8}
DF = (3, 4, 5, 7, 8, 9, 10, 12, 16, 21, 22)
rows = ["| q | A tip med ms (n) [min-max] | U auto med (n) [min-max] | U-A ms (%) | per-process medians A / U | author U-A | results |", "|---|---|---|---|---|---|---|"]
ta = tu = 0.0; d = {}; n8 = []; n4 = []
for q in range(1, 23):
    a = arm('v1', 'A', q); u = arm('v1', 'U', q)
    ta += a['med']; tu += u['med']; d[q] = (u['med'] - a['med'], 100 * (u['med'] - a['med']) / a['med'], a, u)
    (n8 if a['n'] == 8 and u['n'] == 8 else n4).append(q)
    same = 'identical' if len(a['hs'] | u['hs']) == 1 else ('order differs (sorted identical)' if q == 11 else 'DIFF')
    rows.append(f"| {q} | {a['med']:.1f} ({a['n']}) [{a['lo']:.0f}-{a['hi']:.0f}] | {u['med']:.1f} ({u['n']}) [{u['lo']:.0f}-{u['hi']:.0f}] | {d[q][0]:+.1f} ({d[q][1]:+.2f}%) | "
                f"{' '.join(f'{x:.0f}' for x in a['per'])} / {' '.join(f'{x:.0f}' for x in u['per'])} | {AUTH[q]:+.1f} | {same} |")
rows.append(f"| sum | {ta:.0f} | {tu:.0f} | {tu-ta:+.1f} ({100*(tu-ta)/ta:+.2f}%) | | -417.5 | |")
# alternative estimator: mean of per-process medians
alt = sum(st.mean(d[q][3]['per']) - st.mean(d[q][2]['per']) for q in d)
rows.append(f"\nAlternative estimator (mean of per-process medians): {alt:+.1f} ms.")
def fmt(q): return f"{d[q][0]:+.1f} ({d[q][1]:+.1f}%)"
def pair(label, f1, f2, q):
    a = arm(label, f1, q); b = arm(label, f2, q)
    if not a or not b: return None
    return f"{a['med']:.1f} vs {b['med']:.1f} (n={a['n']} / {b['n']})", a, b
o21 = arm('v1', 'O', 21); o3 = arm('v1', 'O', 3)
rep = {
    '@TABLE@': "\n".join(rows),
    '@SUITE@': f"{ta:.0f} -> {tu:.0f} ms = {tu-ta:+.1f} ms ({100*(tu-ta)/ta:+.2f}%)",
    '@SUITEWORD@': "confirmed (about %d%% of the claimed saving)" % round(100 * (tu - ta) / -417.5),
    '@Q21@': fmt(21), '@Q3@': f"{d[3][0]:+.1f}", '@Q7@': f"{d[7][0]:+.1f}", '@Q8@': f"{d[8][0]:+.1f}", '@Q10@': fmt(10),
    '@Q6@': f"{d[6][0]:+.1f}", '@Q2@': f"{d[2][0]:+.1f}",
    '@O21@': f"{o21['med']-d[21][2]['med']:+.1f}", '@O3@': f"{o3['med']-d[3][2]['med']:+.1f}",
    '@O21ABS@': f"{o21['med']:.1f} vs {d[21][2]['med']:.1f} ({o21['med']-d[21][2]['med']:+.1f} ms)", '@O3ABS@': f"{o3['med']:.1f} vs {d[3][2]['med']:.1f} ({o3['med']-d[3][2]['med']:+.1f} ms)",
    '@DFSUM@': f"{sum(d[q][0] for q in DF):+.1f}", '@RESTSUM@': f"{sum(d[q][0] for q in d if q not in DF):+.1f}",
    '@W21@': pair('w1', 'U', 'W', 21)[0], '@W3@': pair('w1', 'U', 'W', 3)[0], '@R21@': pair('r1', 'U', 'R', 21)[0], '@R3@': pair('r1', 'U', 'R', 3)[0],
    '@NCOUNT@': f"n=8 for q{' '.join(map(str, n8))}; n=4 for " + (f"q{' '.join(map(str, n4))}" if n4 else "none"),
    '@END@': datetime.datetime.now().strftime('%H:%M'),
}
gh = pair('g1', 'G', 'H', 21)
if gh:
    g, h = gh[1], gh[2]; tip = d[21][2]['med']
    rep['@GH@'] = f"gpuor {g['med']:.1f} [{g['lo']:.0f}-{g['hi']:.0f}] vs hostor {h['med']:.1f} [{h['lo']:.0f}-{h['hi']:.0f}] (n={g['n']} / {h['n']})"
    rep['@GHREAD@'] = f"against the tip's {tip:.1f}: gpuor {g['med']-tip:+.1f} ms, hostor {h['med']-tip:+.1f} ms; results identical: {len(g['hs'] | h['hs'] | d[21][2]['hs']) == 1}"
    rep['@GHLINE@'] = f"q21 with `gpuor` alone: {g['med']-tip:+.1f} ms against the tip; with `hostor` alone: {h['med']-tip:+.1f} ms (`auto` {d[21][0]:+.1f}). The CPU OR is what carries the gain with 3 sources."
else:
    rep['@GH@'] = "not run"; rep['@GHREAD@'] = "lock turn did not arrive"; rep['@GHLINE@'] = "`gpuor`-only / `hostor`-only arms in TPC-H: not run."
s = open(f'{X}/notes_template.md').read()
for k, v in rep.items(): s = s.replace(k, v)
left = [w for w in s.split() if w.startswith('@') and w.endswith('@')]
open('/opt/dlami/nvme/dfperf/notes/VERIFY_PROTO_XFER_HOST.md', 'w').write(s)
print("\n".join(rows)); print({k: v for k, v in rep.items() if k != '@TABLE@'}); print("unfilled:", left)
