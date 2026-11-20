#!/bin/bash
# checks.sh : process counts / rc, result identity, error scan, gate-log totals for the 2026-11-19 re-run (CPU only).
R=/opt/dlami/nvme/dfperf; E=$R/runs/eager; cd $E
echo "processes per tag (rc=0 / all):"
for t in v1 b2 c3 c4 fs fs2 gt; do a=$(cat ${t}_*/status.txt 2>/dev/null | wc -l); z=$(cat ${t}_*/status.txt 2>/dev/null | grep -c " rc=0 "); echo "  $t $z / $a"; done
echo "result identity (all arms of all tags vs tip, per query; q11 compared sorted):"
python3 - <<'PY'
import glob, hashlib, collections
E = "/opt/dlami/nvme/dfperf/runs/eager"; bad = 0; n = 0
for q in range(1, 23):
    h = collections.Counter(); files = 0
    for f in glob.glob(f"{E}/*/tpch_*_q{q}/sirius/q{q}/result*"):
        lab = f.split('/')[-5]
        if lab.split('_')[0] not in ('v1', 'b2', 'c3', 'c4', 'fs', 'fs2', 'gt'): continue
        data = open(f, 'rb').read()
        if q == 11: data = b"\n".join(sorted(data.split(b"\n")))
        h[hashlib.md5(data).hexdigest()] += 1; files += 1
    n += 1
    if len(h) != 1: bad += 1; print(f"  q{q}: {len(h)} distinct results over {files} files: {dict(h)}")
print(f"  {n-bad}/{n} queries identical across every process")
PY
echo "log scan (all tags):"
L=$(ls -d v1_* b2_* c3_* c4_* fs_* fs2_* gt_* 2>/dev/null)
echo "  fallback lines: $(for d in $L; do cat $d/tpch_*/log_dir/sirius_*.log 2>/dev/null; done | grep -cE 'execution fallback|fallback to DuckDB|falling back')"
echo "  error / OOM lines: $(for d in $L; do cat $d/tpch_*/log_dir/sirius_*.log 2>/dev/null; done | grep -cE 'Error in Sirius|rmm::bad_alloc|out of memory|illegal memory access|\[eager\].*failed')"
echo "  OOM retries: $(for d in $L; do cat $d/tpch_*/log_dir/sirius_*.log 2>/dev/null; done | grep -c 'reschedule (retry')"
echo "  [eager] activated lines: $(for d in $L; do cat $d/tpch_*/log_dir/sirius_*.log 2>/dev/null; done | grep -c '\[eager\] join .* probe activated'), held: $(for d in $L; do cat $d/tpch_*/log_dir/sirius_*.log 2>/dev/null; done | grep -c '\[eager\] join .* probe held'), session_terminal=false among activated: $(for d in $L; do cat $d/tpch_*/log_dir/sirius_*.log 2>/dev/null; done | grep '\[eager\] join .* probe activated' | grep -c 'filter_plan=true, session_terminal=false')"
