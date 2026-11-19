# proto/op4-filter-kind (worktree `$R/wt-op4`) - RECOVERED 2026-11-19

Patch: `proto-op4-filter-kind.patch` = `git diff pr2570` (base `ae939824`), 4 files, +1321 / -2.
Recovery status: EXACT. Replayed from transcripts; the diffstat matches the original
`git diff --stat HEAD~2` output line for line (cmake +42, bloom_sizing.hpp 17, construction_plan.cpp +18,
df_bloom_bench.cu 1246 lines). No hunk inferred. Tree was clean at the end (verifier: "clean tree") - no
uncommitted work existed.

## Commits (original -> recovered, local only, never pushed)
1. `14b78909` -> `e1a315a5`: cherry-pick of `mb/bloom` (`f5ce0a37`): `test/bench/df_bloom_bench.cu` (875 lines) +
   `df_bloom_bench` target appended to `cmake/sirius-tests.cmake`. Replayed from the mb-bloom agent's
   Write + 2 python edits + cmake heredoc (run wf_305bdc58-6ca, agent a84e5b5b40fbb8fa1).
2. `843cece7` -> `7f1e2f6f`: prototype.
   - `src/op/dynamic_filter/construction_plan.cpp`: OP4 switch.
   - `src/op/dynamic_filter/bloom_sizing.hpp`: OP5 switch (`bloom_blocks_for` no longer constexpr).
   - `test/bench/df_bloom_bench.cu`: `suite=setprobe` (+371 lines): exact hash IN-list probe vs Bloom probe,
     variants `prod,dh64,dh32,lp64,lp32,pow2,bloomprod,bloom,bloomset,bloompow2`.

## Switches (env, read once per process)
- `SIRIUS_PROTO_BLOOM_RANGE=lo:hi` - publish a Bloom instead of a hash IN-list for builds of lo..hi valid keys
  (used: `100000:3000000`; `1:3000000` for the log check). Unset = tip behaviour.
- `SIRIUS_PROTO_BLOOM_BPK=8|16|32` - Bloom bits per key (default 16).

## Build / run (scripts under `$R/runs/op4/bin`, copies in `../scripts/runs/op4/bin`; all under `flock $R/machine.lock`)
- Needs `git submodule update --init --recursive` in the worktree first (not done by recovery). Not built.
- `build.sh` (pixi run make), `build_target.sh df_bloom_bench`.
- Microbench: `run_cases.sh mb/c_set1.txt mb/r_set1.txt`; `show.py mb/r_set1.txt`; `prof.sh nsys|ncu <name> <args>`.
  Args: `suite=setprobe label= key=32|64 probe_key= members= universe= hit_permille= prior_permille= run=
  ascending= factor= bpk= reps= variants=`.
- Queries: `B_REPO=$R/wt-op4 B_ENV="SIRIUS_PROTO_BLOOM_RANGE=100000:3000000" abba.sh kind 6 17 8 11 21 19 7 20 18`;
  `B_ENV="SIRIUS_PROTO_BLOOM_BPK=8" abba.sh bpk8 6 21 3 7 10 4 9 8 5 12 16 22 20`; `par.sh`, `nsys_block.sh`,
  `table.py <tag> <q...>`, `dfsum.py <nsys.sqlite...>`, `unit.sh "<catch2 filter>"`.
- Verifier scripts: `$R/runs/vop4/bin` (one_query, abba, logpass, unit, wave1, wave2, wave_unit, table, qsplit, lpdiff).

## Result (see notes/PROTO_OP4.md, notes/VERIFY_PROTO_OP4.md)
OP4: drop (no wall gain). OP5 (8 bits/key): gain is build-site/finish-chain time only; not for speed.

## Not recovered
Raw data (runs/op4/{mb,kind_*,bpk8_*,nsys_*,log_*}, runs/vop4/*_table.md, telemetry), builds. Ad-hoc inline
python analysis snippets (never saved as files) are only in the transcripts.
