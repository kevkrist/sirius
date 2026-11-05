#!/usr/bin/env python3
"""pw_side.py q dir=name ... : side-by-side exclusive pipeline wall (pipe_wall.py) across runs, matched by label."""
import sys, subprocess, collections
q = sys.argv[1]; runs = [a.split("=") for a in sys.argv[2:]]
tab = collections.OrderedDict(); 
for d, n in runs:
    out = subprocess.run([sys.executable, "10-analysis/fam_semijoin/pipe_wall.py", d, q], capture_output=True, text=True).stdout
    for line in out.strip().splitlines():
        pid, lab, w, p = line.split(",")
        tab.setdefault((int(pid), lab), {})[n] = (float(w), float(p))
names = [n for _, n in runs]
print(f"{'pid label':<58}" + "".join(f"{n:>13}" for n in names))
for (pid, lab), v in sorted(tab.items()):
    if max(x[0] for x in v.values()) < 3: continue
    print(f"{pid:<3}{lab[:55]:<55}" + "".join((f"{v[n][0]:7.1f}({v[n][1]:4.0f})" if n in v else " " * 13) for n in names))
