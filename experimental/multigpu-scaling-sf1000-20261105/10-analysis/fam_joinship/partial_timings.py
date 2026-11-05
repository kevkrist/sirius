#!/usr/bin/env python3
"""partial_timings.py <run_dir>: same parsing as run_session.sh, for a run still in progress."""
import re, sys
out = sys.argv[1]; cur = None; last = None; rows = []
for line in open(f"{out}/output.txt", errors="replace"):
    m = re.match(r"__(Q\d+_ITER\d+|PIN_BEGIN|PIN_END)__", line.strip())
    if m:
        if cur and last is not None: rows.append((cur, last))
        cur = m.group(1); last = None; continue
    m = re.search(r"Run Time \(s\): real ([0-9.]+)", line)
    if m: v = float(m.group(1)); last = (max(last or 0, v) if cur and not cur.startswith("PIN") else (last or 0) + v)
if cur and last is not None: rows.append((cur, last))
for k, v in rows: print(k, v)
