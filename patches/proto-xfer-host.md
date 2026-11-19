# proto/xfer-host (worktree wt-xfer-host) - RECOVERED WORK IN PROGRESS, transcript wf_7ba57d62-346 agent a087f263dc15a4632

Patches: `proto-xfer-host.patch` = `git diff pr2570 proto/xfer-host` (includes the mb/lifecycle bench commit it was stacked on);
`proto-xfer-host.on-mb-lifecycle.patch` = only this prototype's own delta (12 files, +1534/-11).
Local commits in R: a57417f7 (cherry-pick of recovered mb/lifecycle b31ca131, orig a5b37728) + 336915e1 (orig 3ef18e27,
"proto(xfer-host): pipelined HOST-staged Bloom finish, stage-once replica fan-out, batched exact-key gather").

Status of every hunk: EXACT (2 Write files, 11 python patch scripts with their asserts, 18 KB host_pipeline class, 1 sed;
cross-checked against the transcript's `git diff` of device_transfer.cpp, the +/- lines of exact_key_stager.cu and a 120-line
read of the .cu: 0 diffs). Same caveat as d2d for bench_dynamic_filter_lifecycle.cpp (depends on recovered mb/lifecycle).

New files: src/memory/proto_xfer.hpp (switch parsing), src/cuda/proto_xfer.cpp (AVX-512 host OR + worker threads; added to
src/cuda/CMakeLists.txt). Changed: sirius_dynamic_bloom_filter.cu (host_pipeline: pipelined HOST slots, GPU-OR or host-OR,
checksum), device_transfer.{hpp,cpp} (stage-once bounce for replica fan-out), exact_key_stager.cu (batched gather),
in-list filters, replica_transfer, bench (MB4P + `[xfer_proto]` identity test).

How far it got when the session died (03:17 UTC, still polling the queue):
- Design: DECIDED, written, committed locally.
- Code compiling: NO. Never built. stage1.sh (build + identity check + short MB4P) waited > 2 h for the machine lock and never
  started. One compile fix was made by reading headers only (`entry->stream_ref()`). Expect compile errors.
- Measured: NOTHING (no MB4P, no TPC-H ABBA, no nsys). notes/PROTO_XFER_HOST.md (EXACT, written 01:19) is design + plan only.

Switches (unset = tip paths unchanged): SIRIUS_PROTO_XFER_HOST=gpuor|hostor|auto, SIRIUS_PROTO_XFER_REPLICA,
SIRIUS_PROTO_XFER_WINDOW, SIRIUS_PROTO_XFER_OR_THREADS, SIRIUS_PROTO_XFER_CHECKSUM, SIRIUS_PROTO_XFER_FAIL_HOST.
Bench env: SIRIUS_BENCH_DF_{GPUS,MB,MODES,SOURCES,WINDOWS,OR_THREADS,FORCE_HOST}. Tests: `[xfer_proto]`, `[mb4p]`.

Scripts (runs/xfer_host/bin, all EXACT): env.sh, build.sh, build_only.sh, stage1.sh, ut_run.sh, ut_df.sh, one.sh, pair.sh,
abba_block.sh, nsys_block.sh, mb_all.sh, mb_full.sh, mb_g2.sh, mb_exact.sh, abba_table.py, mb_table.py, nsys_sum.py,
queue = next.sh/waiter.sh + runs/xfer_host/queue.txt (EXACT, the 16 planned jobs; none ran).
Intended order: queue.txt top to bottom (stage1.sh sets gate.ok; GATE lines need it). Needs submodules + `pixi install` first.
