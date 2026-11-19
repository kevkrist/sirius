# mb-lifecycle.patch  (branch mb/lifecycle, worktree R/wt-mb-life; R=/opt/dlami/nvme/dfperf)

Recovery status: EXACT. Replayed from transcript wf_305bdc58-6ca / agent af97d89ff36c5d76e
(python edit #27, Write #28, sed #35, python edit #52 [7 TEST_CASE matches, same as original]).
diff --stat for the two edited files matches the transcript (cmake +1; complete_build_inventory.cpp +10/-3).
Base: pr2570 @ ae939824. Original commit a5b37728 (one commit); recovered commit b31ca131 on mb/lifecycle.
Only this agent's state: MB8 etc. added later by other agents are NOT here.

Files
- test/cpp/operator/bench_dynamic_filter_lifecycle.cpp (new): Catch2 cases in sirius_unittest
  [!benchmark][dynamic_filter] + tag per case: [mb0] primitives, [mb2] contribution fixed cost,
  [mb3] begin, [mb4] Bloom finish, [mb4x] exact slices at finish, [mb5] small build vs one shot,
  [mb7] bounds vs rows. Each case holds a trace_flush (cudaProfilerStop at end, needed for nsys). Prints "ROW,<case>,k=v,..." lines.
- cmake/sirius-test-sources.cmake: +1 line registering the bench.
- src/op/dynamic_filter/complete_build_inventory.cpp: PROTOTYPE env override
  SIRIUS_PROTO_BLOOM_CHUNK_MIB = largest Bloom transfer chunk (MiB), + #include <cstdlib>.

Env vars: SIRIUS_BENCH_DF_GPUS, _MB (list), _REPS, _KINDS (bloom|auto|...), _KEYS, _ROWS, _BATCHES,
_TOTAL_ROWS, _CHUNK_MIB; SIRIUS_PROTO_BLOOM_CHUNK_MIB.

Build / run (scripts in scripts/mblife, restored to R/mblife)
- worktree needs `git submodule update --init --recursive` first (not done in recovery).
- flock R/machine.lock R/mblife/env.sh R/mblife/build.sh        (builds sirius_unittest)
- flock R/machine.lock R/mblife/run.sh <label> <gpus> "<catch filter>" [VAR=VALUE...]  (NSYS=1 -> nsys); log in R/mblife/out/<label>.log
  e.g. run.sh g1_all 0 "[mb0],[mb2],[mb3],[mb4],[mb4x],[mb5],[mb7]" SIRIUS_BENCH_DF_GPUS=1 SIRIUS_BENCH_DF_MB=1,16,128,512,1024,3072,12288
- tab.py <log> <case> col1,col2 [k=v filters] ; nsys_batch.sh (BLIST=8 optional) ; api.py / api8.py <cfg...> (nsys API counts per attempt)

Not recovered: raw logs R/mblife/out, nsys traces, build logs (numbers are in notes/MB_LIFECYCLE.md).
Not compiled after recovery.
