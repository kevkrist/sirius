# proto/xfer-d2d (worktree wt-xfer-d2d) - RECOVERED WORK IN PROGRESS, transcript wf_7ba57d62-346 agent a5f5bb815096ad9d6

Patches: `proto-xfer-d2d.patch` = `git diff pr2570 proto/xfer-d2d` (includes the mb/lifecycle bench commit it was stacked on:
cmake/sirius-test-sources.cmake, complete_build_inventory.cpp, bench_dynamic_filter_lifecycle.cpp base);
`proto-xfer-d2d.on-mb-lifecycle.patch` = only this prototype's own delta (6 files, +731/-26).
Local commits in R: 09a04c32 (cherry-pick of recovered mb/lifecycle b31ca131, orig a5b37728) + c15b053e (prototype).
Originally the prototype edits were UNCOMMITTED in the worktree; the recovery commit is new.

Status of every hunk: EXACT (all 5 python patch scripts + the 20.8 KB island_reduce class replayed with their own asserts;
3 transcript outputs after the patches - grep line numbers, include list, a 77-line sed - match 0 diffs).
Caveat: exactness of bench_dynamic_filter_lifecycle.cpp rests on the recovered mb/lifecycle commit being exact.

How far it got when the session died (01:24 UTC, last call = queue set-up, no result returned):
- Design: DECIDED and fully written. `accumulated_bloom_builder::impl::island_reduce` in sirius_dynamic_bloom_filter.cu:
  group GPUs into P2P islands, peer copies inside each island, HOST legs only between island leaders; `star` = today's HOST
  loop between leaders, `exchange` (default when on) = two leaders exchange both ways at once. Plus `detail::replica_relay`
  for replica fan-out (in-list / small-in-list / replica_transfer) and MB4 mask/mode loop + bit-identity gate test (MBID).
- Code compiling: NO. Never built: the first build waited on the machine lock (~115 waiters) and was killed; the rebuild was
  queued (todo.txt) and never got the lock. Expect compile errors.
- Measured: NOTHING. No gate, no MB4, no TPC-H. No notes file was written (PROTO_XFER_D2D.md never existed).

Switches: `SIRIUS_PROTO_XFER_D2D` = unset/0/off (tip behaviour) | star | 1/exchange. Bench env: SIRIUS_BENCH_DF_GPUS,
SIRIUS_BENCH_DF_MASKS, SIRIUS_BENCH_DF_MB (default changed to 1,3,20,57), SIRIUS_BENCH_DF_ROWS, SIRIUS_BENCH_DF_XFER.
Tests: `[mbid]` (gate), `[!benchmark][dynamic_filter]` MB4.

Scripts (runs/xfer_d2d/bin, all EXACT heredocs): build.sh, rebuild.sh, one_query.sh, abba_q.sh, abba_guard.sh, mb.sh, mb4.sh,
gate.sh, nsys_q.sh, queue = next.sh/slot.sh/add.sh + todo.txt (RECONSTRUCTED from the add.sh calls: build, gate, mb4, abba a1 6 21,
abba a1 6 3). abba_table.py = DERIVED (`sed s#runs/op2#runs/xfer_d2d#g` of the recovered runs/op2/bin/abba_table.py).
MISSING: runs/xfer_d2d/bin/nsys_df.py (plain copy of runs/profile/bin/nsys_df.py; take it from that group's recovery).
Intended order: build.sh -> gate.sh (creates GATE_OK) -> mb4.sh -> abba_guard.sh. Needs `git submodule update --init --recursive` first.
