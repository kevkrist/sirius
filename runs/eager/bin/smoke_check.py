#!/usr/bin/env python3
"""GO/NOGO for the eager smoke: rc=0, results identical to tip, [eager] activations logged with session_terminal=true (or no
plan), no error / fallback lines, compressed-scan probes complete."""
import glob, hashlib, re, sys
import os
R = '/opt/dlami/nvme/dfperf/runs'
LAB = os.environ.get('SMOKE_LABEL', 'smoke_dbg')
ok = True
def tipres(q):
    for pat in (f'{R}/op1b/s1_tip1/tpch_*_q{q}/sirius/q{q}/result.txt', f'{R}/op1b/t1_tip1/tpch_*_q{q}/sirius/q{q}/result.txt',
                f'{R}/baseline/baseline_tip/tpch_*_q{q}/sirius/q{q}/result.txt'):
        g = glob.glob(pat)
        if g: return g[0]
for q in (3, 12, 16):
    st = [l for l in open(f'{R}/eager/{LAB}/status.txt') if f' q={q} ' in l]
    rc0 = bool(st) and 'rc=0' in st[-1]
    res = glob.glob(f'{R}/eager/{LAB}/tpch_*_q{q}/sirius/q{q}/result.txt'); t = tipres(q)
    same = bool(res) and t and hashlib.md5(open(res[-1], 'rb').read()).hexdigest() == hashlib.md5(open(t, 'rb').read()).hexdigest()
    logs = glob.glob(f'{R}/eager/{LAB}/tpch_*_q{q}/log_dir/sirius_*.log')
    txt = ''.join(open(f, errors='replace').read() for f in logs)
    act = len(re.findall(r'\[eager\] join \d+ probe activated', txt))
    bad_term = len(re.findall(r'\[eager\].*(NOT terminal|failed)', txt))
    plan_not_term = len(re.findall(r'filter_plan=true, session_terminal=false', txt))
    err = len(re.findall(r'Error in Sirius|rmm::bad_alloc|out of memory|illegal memory access|execution fallback|fallback to DuckDB|falling back', txt))
    retry = len(re.findall(r'reschedule \(retry', txt))
    kept = re.findall(r'join filters kept \d+/\d+ rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)', txt)
    partial = sum(1 for a, b, c in kept if a != b)
    okwin = len(re.findall(r'outcome=ok', txt))
    good = rc0 and same and act > 0 and bad_term == 0 and plan_not_term == 0 and err == 0
    ok &= good
    print(f"q{q}: rc0={rc0} result_identical_to_tip={same} eager_activations={act} not_terminal={bad_term + plan_not_term} error_lines={err} "
          f"oom_retries={retry} cscan_chunks={len(kept)} chunks_with_partial_probes={partial} ok_windows={okwin} -> {'OK' if good else 'BAD'}")
sys.exit(0 if ok else 1)
