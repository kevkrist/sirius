# mb/transfer (worktree wt-mb-xfer) - RECOVERED from transcript wf_7ba57d62-346 agent a140a5c89ec393f46

Patch: `mb-transfer.patch` = `git diff pr2570 mb/transfer` (pr2570 = ae939824). Local commit in R: 4eb4e6d5 (orig 3edfefa4).
One commit, adds only `bench/transfer/` (standalone, no engine code touched):

| file | status | how recovered |
|---|---|---|
| xfer_bench.cpp (1493 lines) | EXACT | Write + 3 sed + 2 python patches replayed; line count and 316 lines cross-checked against later reads by another agent: 0 diffs |
| xfer_kernels.cu | EXACT | heredoc |
| tab.py | EXACT | Write + 1 python patch |
| build.sh | EXACT | heredoc |
| run_all.sh, run_cont2.sh | EXACT | copies of runs/mb_xfer/bin/{all.sh,cont2.sh} at commit time |

State: the committed xfer_bench.cpp includes the `cont2` / `bgleg` follow-up patch, which was NEVER compiled or run
(all numbers in notes/MB_TRANSFER.md come from the source before that patch; to get it: revert the hunk set described in
MB_TRANSFER.md section 10, i.e. `tests=cont2`, `g_bgleg`, `run_df` mode 2, HSZ = 2 x BMAX).

Build / run (needs R/repo's pixi env: nvcc, rmm; GPU job, take the machine lock):
```bash
R=/opt/dlami/nvme/dfperf
flock $R/machine.lock $R/runs/mb_xfer/bin/all.sh      # phases build,smoke,main,nopeer,probe,nsys; resumable (out/.ok_<phase>); PHASES=, REPS=, MAINARGS=, MAINLOG=
flock $R/machine.lock $R/runs/mb_xfer/bin/cont2.sh    # follow-up, never ran
python3 $R/wt-mb-xfer/bench/transfer/tab.py $R/runs/mb_xfer/out/main.log df   # leg|pair|fan|hostor|conc|df|cont
$R/runs/mb_xfer/bin/xfer_bench tests=df dfsizes=281 reps=5   # keys: tests sizes dfsizes fansizes reps bmax nopeer probe bgleg pairmethods
```
Build falls back to a non-RMM build (-DUSE_RMM off) if linking rmm fails.

Notes: notes/MB_TRANSFER.md - EXACT except section 7 (contention), where 4 leg rows (bg=p2p01+host02), 1 cell (star_peerapi B=1434, same bg) and 1 background-throughput bullet were produced
from data that never appeared in the transcript; they are marked `[LOST: not in transcript]`. All other tables were taken from
captured outputs / later reads (sections 0, 4, 6 verified byte-for-byte against another agent's read; total line count and
heading line numbers match the original).
Raw data (runs/mb_xfer/out, nsys capture, topo.txt) is lost.
