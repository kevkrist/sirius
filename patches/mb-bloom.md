# mb-bloom.patch  (branch mb/bloom, worktree R/wt-mb-bloom; R=/opt/dlami/nvme/dfperf)

Recovery status: EXACT. Replayed from transcript wf_305bdc58-6ca / agent a84e5b5b40fbb8fa1
(Write #30, cmake append #31, python in-place edits #34 and #50; all 7 replace() targets verified present).
Base: pr2570 @ ae939824. Original commit f5ce0a37 (one commit); recovered commit 80958cc8 on mb/bloom.
Only this agent's state: later extensions by other agents (suite=setprobe, transfer bench, MB8...) are NOT here.

Files
- test/bench/df_bloom_bench.cu (new, ~36 KB): standalone bench. MB1 insert via
  accumulated_bloom_builder::enqueue_add; MB6 probe + measured FPR via
  sirius_dynamic_bloom_filter::compute_mask; throwaway kernel variants; hash_partition overlap modes.
- cmake/sirius-tests.cmake: appended target df_bloom_bench (output build/release/test/bench/df_bloom_bench).

Args (key=value on command line)
- suite=insert|probe, label=, variant=, mode=alone|hp_only|hp_same|hp_second, order=random|seq
- array_mib, bpk, batch, key=64|32, nulls, dup, min_batches, max_batches, checksum, parts, payload,
  probe, reps, sorted_probe, gpu_gib
- variants: prod cuco_addif cuco_add cuco_cond cuco_v8 cuco_v8c w64 w64_1024 w64c w64v pairs sortpairs
  sorted diag_read diag_rmw blk64 blk64c
- Output: "RES,insert,..." / "RES,probe,..." lines.

Build / run (scripts in scripts/runs/mb_bloom, restored to R/runs/mb_bloom)
- worktree needs `git submodule update --init --recursive` first (not done in recovery).
- build_locked.sh df_bloom_bench  (flock R/machine.lock; build.sh configures preset release w/ sccache+mold)
- flock R/machine.lock ./run_cases.sh <abs cases file> <abs out file>   (cases: c_check c_A c_BCDE c_F c_G c_H c_P)
- show.py <r_file> col1,col2,...   ; prof_nsys.sh <name> <args> ; q.py <sqlite> ; prof_ncu.sh (ncu failed: ERR_NVGPUCTRPERM)

Not recovered: raw results r_*.txt, nsys/ncu output, build logs (data; numbers are in notes/MB_BLOOM.md).
Not compiled after recovery.
