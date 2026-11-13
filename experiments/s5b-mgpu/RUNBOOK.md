# S5b multi-GPU experiment runbook

This runbook is for a Claude Code session on a machine with 4 PCIe GPUs. It checks the S5b stack
(PR1 #2569 and PR2 #2570: unified Dynamic Filter Construction at the build PARTITION; PR3-A #2604:
the build fold rule) for correctness on several GPUs first, then for performance, and picks the
build fold policy. You run it stage by stage. **At the end of every stage, report to the user and
wait for approval before you continue.**

Sources of truth: `S5B_SPEC.md` rev 3.3 (Changelog "Revision 3.3", "Build fold rule (PR3,
decision A)", "Testing Decisions"). The user has these; you do not need them to follow this
runbook.

Vocabulary follows `src/op/dynamic_filter/GLOSSARY.md` (binding) and `docs/glossary.md`. The terms
used here are: **Dynamic Filter**; **Bloom Filter**; **IN-list**; **Zone Map**; **Admitted Key**;
**Key Outcome**, the final result for one Admitted Key, which Sirius logs as one
`[dynamic_filter_key]` debug line; **Replica**, a copy of a published Dynamic Filter on one GPU;
**Publication Session**, one join's lifecycle of building its Dynamic Filters and publishing them;
**Contribution**, one task adding its batch to Construction.

## Rules

1. **Run every command through pixi.** The stage scripts are plain bash orchestrators
   (`bash experiments/s5b-mgpu/bin/<script>.sh ...`); they run every tool through `pixi run` in the
   right worktree. Run any ad-hoc command yourself as `pixi run <cmd>` from a worktree.
2. **One Sirius process at a time; never start one while another is running.** This includes
   DuckDB CPU work (the CPU reference) and the unit tests. The scripts refuse to start a step while a
   `duckdb` or `sirius_unittest` process, a GPU process or another step is running, and tell you
   so: then STOP and ask the user; do not kill processes you did not start. The only parallelism is
   inside the unit-test harness of C1: the `full` step runs 8 shard processes at once (2 per GPU),
   and its `cuda_fault` and `late_mat` steps run two jobs internally.
3. **Steps longer than about 10 minutes run in Claude Code's background mode.** Start them with
   the Bash tool's `run_in_background: true`; Claude Code notifies you when the command ends, so
   do not poll in a loop and do not start anything else meanwhile (Rule 2). Every GPU step runs
   as a *step*: it writes `S5B_WORK/steps/<name>.pid`, runs under a hard `timeout` (exit 124 when
   it fires), tees its output to `S5B_WORK/steps/<name>.out`, and writes its exit code to
   `S5B_WORK/steps/<name>.done`. When notified, read the result with
   `bash experiments/s5b-mgpu/bin/steps.sh status` and `bash experiments/s5b-mgpu/bin/steps.sh tail
   <name>`. If you lose track (a new session, a lost notification), run `steps.sh status` before
   anything else: a step shown as `running` is still running; wait for it.
4. **All data is Parquet.** TPC-H only, read through `read_parquet` views. Never generate or use
   `.duckdb` datasets. Query results under `S5B_WORK` are CSV intermediates; they lose no precision.
5. **Ask before anything not in this runbook.** Do not change Sirius code, configs, scripts or
   settings to make a stage pass. If something fails, collect evidence, STOP and report. The steps
   listed in a stage are approved once the user approves the stage.
6. **Never push to `origin`.** Results go only to the user's fork remote (`kevkrist`), on branch
   `tmp/s5b-mgpu-experiments`, and only after the user approves that stage's push.
7. **Results go only to `experiments/s5b-mgpu/results/` and stay small:** Parquet tables,
   `summary.md` files and grepped log excerpts. No full logs, no CSV results, no binaries. Raw
   output lives under `S5B_WORK` (outside the repository). `publish_results.sh` refuses files over
   5 MiB and raw logs.
8. **Report concisely.** At each STOP, give: the verdict against the bar (PASS, FAIL or
   INCOMPLETE), the numbers the stage asks for with their noise, anything unexpected, wall-clock time
   used, the measured disk and memory (Resources), and the next stage's estimate.

## Layout

| What | Where (defaults; override in `experiments/s5b-mgpu/local.env`) |
|---|---|
| Control checkout (this branch, scripts, `results/`) | the clone you are in, on `tmp/s5b-mgpu-experiments` |
| Build worktrees | `S5B_HOME/wt-tip` (b65efbe3), `wt-key` (2b3b1c69), `wt-base` (2b3b1c69^) |
| TPC-H Parquet SF1, SF10, SF100 | `S5B_DATA/tpch_parquet_sf{1,10,100}` or `S5B_TPCH_SF<N>=<dir>` |
| TPC-H Parquet SF1000 | `S5B_TPCH_SF1000` (already generated; the user gives the path) |
| Sirius config | `S5B_SIRIUS_CONFIG`: the machine's own config for 4 GPUs |
| Raw output, configs, queries, CPU answers, step files | `S5B_WORK` (default `S5B_HOME/work`) |
| Results to share | `experiments/s5b-mgpu/results/<stage>/` |

`S5B_HOME` defaults to `~/s5b-mgpu`. The three builds:

| Name | Commit | Role |
|---|---|---|
| `tip` | `b65efbe3` | PR3 commit A, the stack under test |
| `key` | `2b3b1c69` | S5's tip plus only the `[dynamic_filter_key]` line: the Key Outcome baseline |
| `base` | `2b3b1c69^` | S5's tip: the performance baseline |

Scripts: `bin/s0_setup.sh`, `bin/c1_unit_tests.sh`, `bin/c2_key_outcomes.sh`, `bin/c4_sf10.sh`,
`bin/c5_fold_policies.sh`, `bin/perf.sh`, `bin/steps.sh`, `bin/publish_results.sh`,
`bin/cleanup.sh`; Python in `lib/`; the key-parity script in `tools/`. Table schemas live in
`lib/schemas.py` and log-line formats in `lib/formats.py`; Appendices A and C are generated from
them. Every run step is resumable: rerun the same command after an interruption. A finished unit
(a C-stage query, a P-stage session) is skipped; an unfinished one is moved to a `.partial/`
directory and rerun. A summary whose inputs are missing says INCOMPLETE (exit 3), never PASS.

## Prerequisites

1. The user has pushed `tmp/s5b-mgpu-experiments` to their fork. In a Sirius clone with remotes
   `origin` = `sirius-db/sirius` and `kevkrist` = the fork:

   ```bash
   git fetch kevkrist tmp/s5b-mgpu-experiments
   git switch tmp/s5b-mgpu-experiments   # or: git switch -c tmp/s5b-mgpu-experiments kevkrist/tmp/s5b-mgpu-experiments
   ```

   Run every command below from the root of this checkout.
2. `pixi` on `PATH`, network access (pixi packages, submodules, and `tpchgen-rs` if SF1-SF100 data
   must be generated), and 4 idle GPUs.
3. **Ask the user once, in one question**, for (a) the path of the machine's own Sirius config for
   4 GPUs and (b) the path of the existing TPC-H SF1000 Parquet directory, and where SF1, SF10 and
   SF100 Parquet live if they exist. Copy `experiments/s5b-mgpu/local.env.example` to `local.env`
   and set `S5B_SIRIUS_CONFIG`, `S5B_TPCH_SF1000` and any `S5B_TPCH_SF<N>`.

## Resources (measure, then extrapolate)

| Stage | Disk | Host memory |
|---|---|---|
| S0 | 3 worktrees x 30-60 GB; SF1+SF10+SF100 Parquet about 35 GB if generated | builds: one at a time |
| C1 `full` | logs, a few GB | 8 shard processes, each with a pinned host tier capped at 32 GB (`integration-shard.yaml`): at most 256 GB |
| C2, C4, C5 | logs at debug level, a few GB | per process, as the machine config allows |
| CPU reference SF1000 | answers small; DuckDB spill in `S5B_WORK/reference/duckdb_tmp` can reach hundreds of GB on q9, q18, q21 (emptied after each query) | DuckDB `memory_limit` 60% of RAM |
| P1, P3 (SF1000) | each session's logs, gzipped when it ends | every table pinned on the host tier: about the SF1000 uncompressed size (`data.parquet` `uncompressed_bytes`), less if the machine config compresses pinned tables |
| P2 (SF100) | 30 cells of trace-level warm logs, gzipped when the session ends | the SF100 uncompressed size |

The kit measures what it can: `results/S0/data.parquet` has each dataset's uncompressed size;
`reference_sf<N>.parquet` has each CPU query's spill peak; `sessions.parquet` has each P-stage
process's peak resident memory, the host's lowest available memory and the session directory's
peak size. Before the CPU reference at SF1000, check `df -h $S5B_WORK`: with less than 500 GB free,
STOP and ask. After each stage's first session, report its measured disk and memory and the
extrapolation for the rest of the stage.

## Results exchange (every stage)

After a stage's STOP report, and only when the user approves the push:

```bash
bash experiments/s5b-mgpu/bin/publish_results.sh show <stage>     # list files and sizes; show the user
bash experiments/s5b-mgpu/bin/publish_results.sh commit <stage>   # commit results/<stage> only: "chore(s5b-mgpu): add <stage> results"
bash experiments/s5b-mgpu/bin/publish_results.sh push             # push to kevkrist, never origin
```

Commit messages use the Conventional Commits type `chore` and carry no AI attribution or co-author
line.

---

## S0. Setup (about 2-4 h)

Goal: three builds, the data, the configs, and a hardware record.

```bash
bash experiments/s5b-mgpu/bin/s0_setup.sh check       # tools, remotes, 4 GPUs, nothing running, config and SF1000 paths, disk
bash experiments/s5b-mgpu/bin/s0_setup.sh hardware    # -> results/S0/hardware.md
bash experiments/s5b-mgpu/bin/s0_setup.sh fetch       # stack branches; commits; same pixi.lock
bash experiments/s5b-mgpu/bin/s0_setup.sh worktrees   # wt-tip, wt-key, wt-base + submodules
bash experiments/s5b-mgpu/bin/s0_setup.sh build       # BACKGROUND: pixi run make in each, one at a time (30-60 min each)
bash experiments/s5b-mgpu/bin/s0_setup.sh selftest    # host-only checks of the scripts and the parity script
bash experiments/s5b-mgpu/bin/s0_setup.sh data        # checks SF1/10/100/1000 -> results/S0/data.parquet
bash experiments/s5b-mgpu/bin/s0_setup.sh configs     # machine config + 1/2-GPU copies -> results/S0/configs/
bash experiments/s5b-mgpu/bin/s0_setup.sh queries     # 22 queries per SF (Q11 rendered per SF)
bash experiments/s5b-mgpu/bin/s0_setup.sh probe       # per build/config: resolved H, disk and pinned smoke queries
bash experiments/s5b-mgpu/bin/s0_setup.sh summarize   # -> results/S0/summary.md
```

**Configs.** The kit chooses no memory, cache, compression or telemetry setting. `configs` copies
the machine config unchanged as the 4-GPU config, derives the 1- and 2-GPU configs by changing only
`topology.num_gpus` (with `scripts/patch_config.py`), proves that nothing else differs, and copies
all of them (and the machine config) into `results/S0/configs/`. It refuses a machine config that
does not select 4 GPUs or selects them with `gpu_ids`: then STOP and ask.

**Data.** SF1000 is never generated: it must exist at `S5B_TPCH_SF1000`. SF1, SF10 and SF100 are
reused when they exist. If `data` reports one of them as `NOT READY`, do not generate it now:
report it at the STOP. Steps that need a missing scale factor (`queries`, `probe`) fail for that
scale factor only; rerun them after the generation below.

Expected output:

- `check`: `check passed`.
- `fetch`: three lines `tip = b65efbe3...`, `key = 2b3b1c69...`, `base = fec54719...`, then
  `pixi environment identical across builds`.
- `build`: `build <name> ok in <n> s` three times; `results/S0/builds.txt` has `ok=yes` lines.
- `selftest`: two unittest runs ending `OK` (the kit's own checks, then the parity script's
  fixture tests). A failure here means a log pattern, a schema, a summary or the cleanup guard is
  wrong: STOP before any GPU stage.
- `data`: `SF<N>: ok` for every present scale factor, lineitem rows about 6,000,000 x SF, and its
  uncompressed size.
- `configs`: the machine config's memory, cache, compression and Quent settings, then
  `configs ok`.
- `probe`: one line per build/GPU count with `H=<bytes>`, `gpus_seen=0` / `0,1` / `0,1,2,3`
  matching the GPU count, `fallback=False` and `pinned_scans=1` or more.

Pass bar: all of the above. `gpus_seen` must equal the configured GPUs, H must be the same for
`tip` and `base` at a given GPU count, and the pinned scan must be served from the pin (the P
stages count it for every query).

**STOP: report to the user and wait for approval.** Report the GPU model and memory, the PCIe
topology, P2P read/write support between every GPU pair (`hardware.md`), where the data came from
and which scale factors are missing, the machine config's settings as `configs` printed them
(whether Quent telemetry is on and where it writes, and whether pinned tables are compressed), the
SF1000 uncompressed size against host memory, the resolved H per config, and build times.

**After the user approves:** generate each missing scale factor, one at a time, in the background,
then rerun the steps that need it:

```bash
bash experiments/s5b-mgpu/bin/s0_setup.sh generate 100    # BACKGROUND; also 1 and 10 if missing; checks free space first
bash experiments/s5b-mgpu/bin/s0_setup.sh data
bash experiments/s5b-mgpu/bin/s0_setup.sh queries
bash experiments/s5b-mgpu/bin/s0_setup.sh probe           # if SF1 was missing
```

---

## C1. Unit tests at the tip (about 1.5 h)

Goal: every `[multi_gpu]` test, then the full suite, which has never run on several GPUs.

```bash
bash experiments/s5b-mgpu/bin/c1_unit_tests.sh multi_gpu        # BACKGROUND, <= 20 min
bash experiments/s5b-mgpu/bin/c1_unit_tests.sh multi_gpu_peer   # BACKGROUND; only if S0 shows P2P read+write OK on every pair
bash experiments/s5b-mgpu/bin/c1_unit_tests.sh full             # BACKGROUND; pixi run make test with 8 shard processes, 45-75 min
```

`multi_gpu_peer` sets `SIRIUS_TEST_REQUIRE_POOL_PEER_ACCESS=1`, so tests of direct peer copies
fail instead of skipping when the memory pools do not grant peer access. Skip it, and say so,
when `hardware.md` shows P2P unsupported on some pair. `nvidia-smi topo -p2p` only reports what the
driver advertises; Sirius grants peer access only after it verifies copies between its own pools
byte for byte (`docs/super-sirius/multi-gpu-architecture.md`). So a failure in `multi_gpu_peer` on
a pair that `topo -p2p` reports OK may be the hardware or driver, not the code: report it as such.
The forced HOST-staging Bloom Filter transfer is covered by these unit tests (no SQL setting forces
it).

Each step first moves the previous logs under the tip worktree's `build/release/test/cpp/log/` to
`S5B_WORK/C1/stale-<time>/`, so its excerpts come only from its own processes.

Expected output: each step ends with a `Unit test summary` table, every process `exit 0`.
Results: `results/C1/<step>/{summary.txt,catch2_totals.txt,skips_and_failures.txt,console_tail.txt}`.

Pass bar: every process exits 0 in every step run. Record every skipped test with its reason
(`skips_and_failures.txt`); skips are not failures but must be listed.

**STOP: report to the user and wait for approval.** Give the per-process Catch2 totals, the list
of skipped tests with reasons (peer-access skips called out), and any failure excerpt.

---

## C2 (with C3). Key Outcomes on TPC-H SF1, 4 GPUs and 2 GPUs (about 45 min)

Goal: the tip's Key Outcomes are equal to or stronger than the baseline's, every result matches
the CPU with no fallback, and the same holds with Dynamic Filters off. C3 is folded in: report
which path copied Bloom Filter Replicas between GPUs.

```bash
bash experiments/s5b-mgpu/bin/c2_key_outcomes.sh run          # BACKGROUND; 4 GPUs, then 2 GPUs
bash experiments/s5b-mgpu/bin/c2_key_outcomes.sh summarize    # -> results/C2/
```

Per GPU count `<n>` this runs each query in its own DuckDB CLI process that reads Parquet from
disk, at debug log level, once on the GPU and once with `gpu_execution = false`, through
`tools/dynamic_filter_key_parity.py`, with Zone Maps on (`enable_dynamic_zone_map_filter = true`):

- `key_<n>gpu`: the baseline 2b3b1c69 (informational: its own failures do not fail C2, but
  report them);
- `tip_<n>gpu`: the tip;
- `tip_df_off_<n>gpu`: the tip with `enable_dynamic_filter = false`;
- the parity `diff` of `key_<n>gpu` against `tip_<n>gpu` (`results/C2/diff_key_<n>gpu_vs_tip_<n>gpu.txt`).

Expected output: per query `ok q<i>`; each diff ends `summary: equal=<n> ...`.

Pass bar (strict):

- every `tip_*` query: CPU match, no GPU fallback (runtime or plan-time);
- every Key Outcome row `equal`, `stronger` or `extra`, and no Zone Map lost. Any `weaker`,
  `missing` or `duplicate` row, or any lost Zone Map, fails C2, **even when its candidate reason is
  a declared delta** (`bloom_cap`, `declined_pipeline_barrier`, `declined_compressed_arrival`).
  `summary.md` lists each such row and marks the declared ones for the user to judge.

A result that equals the CPU only as a multiset (same rows, other order) passes but is listed
separately: an ORDER BY with ties may reorder rows, or the order may be wrong.

C3, the Bloom Filter transfer path: `summary.md` counts the `bloom_finish` lines (Appendix C) per
label: peer copies are expected when every pool pair grants peer access, HOST staging otherwise;
check it against `hardware.md`. The `publication` line agrees. At SF1 most joins may choose
IN-lists, so few or no Bloom Filters may be published; C4 at SF10 adds evidence. The copy path of
IN-list and Zone Map Replicas is not logged.

If queries fail, the step exits nonzero; continue to `summarize`, then STOP. Rerun failed queries
only when the user asks: `bash experiments/s5b-mgpu/bin/c2_key_outcomes.sh run --retry-failed`.

**STOP: report to the user and wait for approval.** Report per GPU count: CPU match and fallback
(tip, filters off, and baseline separately), the diff status counts and every failing row, the
Key Outcome kind counts, and the Bloom Filter transfer path counts.

---

## C4. TPC-H SF10 on 4 GPUs at the tip (about 20 min)

```bash
bash experiments/s5b-mgpu/bin/c4_sf10.sh run                 # BACKGROUND; --retry-failed on request
bash experiments/s5b-mgpu/bin/c4_sf10.sh summarize           # -> results/C4/
```

Pass bar: every query matches the CPU with no fallback (Parquet read from disk, Zone Maps on).
Multi-partition builds are expected: `summary.md` reports hash joins with more than one partition
(from the `join_strategy` line).

**STOP: report to the user and wait for approval.** Report CPU match, the counts of Key Outcome
kinds (`none`, `bloom`, `small_in_list`, `hash_in_list`) and their reasons, the number of
multi-partition joins, and the Bloom Filter transfer path counts.

---

## C5. Fold policies on 4 GPUs, TPC-H SF10, E2-style settings (about 1 h)

Settings: `max_build_hash_table_bytes = 0` and `concat_batch_bytes = H/8` (H from S0's probe of
the 4-GPU tip), with `build_fold_policy` in {`never`, `legacy_filter_coupled`, `bounded`}.
`build_fold_policy` is test-only: the scripts set `SIRIUS_ENABLE_TEST_OPTIONS=1` and apply it
with SQL `SET`.

```bash
bash experiments/s5b-mgpu/bin/c5_fold_policies.sh run        # BACKGROUND; --retry-failed on request
bash experiments/s5b-mgpu/bin/c5_fold_policies.sh summarize  # -> results/C5/
```

Pass bar:

- every query of every policy matches the CPU with no fallback (Parquet read from disk, Zone Maps
  on);
- every query logged the settings it SET: each `build_fold` line names the label's policy and
  `concat_batch_bytes`, and no join ran BUILD_PROBE (`summary.md`, "logged settings");
- no query has more OOM retries under `legacy_filter_coupled` or `bounded` than under `never`.

Downgrade bytes above `never` are reported, not failed (`summary.md`, "Resources against
`never`"): they vary with timing. Report them for the user to judge.

The `build_fold` lines are in `results/C5/build_folds.parquet`.

**STOP: report to the user and wait for approval.** Report CPU match per policy, OOM retries and
downgrade bytes per policy against `never`, and how many joins each policy folded.

---

## How the P stages measure

P1, P2 and P3 run **sessions**: one long-lived DuckDB process per build and GPU count. A session
loads the extension with its config, creates the Parquet views, and pins every TPC-H table on the
host tier: `CALL pin_table('<files>', tier => 'host', name => '<table>')`, compressed or not as
the machine config says. Then it runs 1 warm pass and 5 hot passes over the 22 queries, rotating
the query order between passes:

- the warm pass logs at debug (P1) or trace (P2, P3), writes each result as CSV and compares it
  with the CPU answer;
- the hot passes log at info and discard results; the latency of a query is the median of its 5
  hot runs (DuckDB CLI `.timer`).

Every run logs to its own directory, so OOM retries, downgrade bytes and Publication Sessions are
counted per query. A run counts only when its execution window ends `ok`, it did not fall back,
and at least one scan was served from a pinned table. GPU peak memory is recorded per process (the
pool's high-water mark), not per query. `nvidia-smi` clocks, temperatures and throttle reasons are
recorded before and after each process (`gpu_state.parquet`).

A session that ends without `record.json` (crash, timeout, a run making no progress for the stall
limit) is set aside and rerun from scratch by the same `run` command. `perf.sh <stage> plan`
prints the sessions, their run counts and a time estimate, measured once a session exists.

**Noise and confirmation.** Every bar is shown with its noise. A query over a bar gets one
confirming re-run, again with a warm pass and 5 hot passes, in new processes; the verdict uses the
confirmed value. `run` starts the confirming sessions by itself when the main sessions are done.
A summary with an unconfirmed query over a bar says INCOMPLETE.

## P1 (with P4). No regression, TPC-H SF1000, host-pinned tables, Dynamic Filters on (about 8-16 h)

Goal: base (`2b3b1c69^`) against tip, on 4 GPUs and on 1 GPU.

```bash
bash experiments/s5b-mgpu/bin/perf.sh P1 reference         # BACKGROUND; CPU answers at SF1000, once (0.5-2 h)
bash experiments/s5b-mgpu/bin/perf.sh P1 plan              # sessions, runs, estimate, SF1000 size against RAM
bash experiments/s5b-mgpu/bin/perf.sh P1 run --gpus 4      # BACKGROUND; about 3-6 h
bash experiments/s5b-mgpu/bin/perf.sh P1 run --gpus 1      # BACKGROUND; about 5-10 h
bash experiments/s5b-mgpu/bin/perf.sh P1 summarize         # -> results/P1/
```

The two `run` commands may go in separate sessions. `reference` runs each query once with plain
DuckDB (`SIRIUS_DISABLE=1`, no extension, Parquet from disk) and stores the answer as Parquet
under `S5B_WORK/reference/sf1000/`; `results/P1/reference_sf1000.parquet` records row counts,
hashes and spill peaks.

Per GPU count, `run` starts three sessions in this order: `base_a`, `tip`, `base_b`. The two base
sessions are an A/A pair: `base_b / base_a` per query is that query's noise, and the base latency
is their geometric mean. A query whose tip/base ratio is over 1.10 is confirmed by two more
sessions over the flagged queries only (`base_confirm`, `tip_confirm`).

Expected output: per session `<stage>/<session>: ok`; progress lines every 5 minutes.

Pass bar, per GPU count:

- base and tip results identical (canonical hash, or equal within float tolerance), and both
  match the CPU answer; no run failed, fell back or read from disk;
- geomean of tip/base median latency <= 1.02, shown with the A/A geomean and the range of the
  per-pass geomeans;
- no query slower than 1.10x after confirmation, shown with the A/A noise per query.

Report q2, q15 and q21 separately (`summary.md` lists them). `summary.md` also compares base and
tip per GPU count on peak GPU memory (per process), OOM retries and downgrade bytes: this is
information only, not a pass bar.

**P4, alongside**, from the 4-GPU tip session (`summary.md`, section P4):

- Publication Sessions, how many sent Bloom Filters through HOST staging, and the publication
  latency after the final Contribution (p50, p95, max), from the info-level `publication` line, for
  the warm pass (debug log, includes one-time costs) and for the hot passes separately. Lines with
  0 filters pushed or no latency are counted and left out of the latency figures;
- Bloom Filter reductions by transfer path (peer copies or HOST staging).

The counters `accumulation_publication_latency_ns` and `accumulation_publications_host_staged`
exist in `dynamic_filter_stats` but Sirius exposes them only to C++ tests (no SQL function, no
log); the log lines above carry the same information.

**STOP: report to the user and wait for approval.** Report per GPU count: the verdict lines,
geomean with its noise, worst query with its noise and any confirmation, q2/q15/q21 ratios, any
non-identical or multiset-only result, the memory comparison (information only), throttle reasons
other than idle, and the P4 numbers.

---

## P2. Fold matrix E1/E2, TPC-H SF100, 1 GPU, tip, host-pinned tables (about 3-8 h)

Matrix (spec rev 3.3), H = `hash_partition_bytes` of the 1-GPU tip config:

- `concat_batch_bytes` in {H/2, H/8, 0} (0 means 0 bytes);
- `build_fold_policy` in {`never`, `legacy_filter_coupled` (filters on only), `bounded`};
- Dynamic Filters on and off (`enable_dynamic_filter`);
- E1 at the resolved `max_build_hash_table_bytes`; E2 with it at 0.

That is 30 cells x 22 queries, in one session (`matrix_1gpu`): the cells are applied with `SET`
(every cell sets all four settings explicitly), in shuffled blocks. Each pass is a block that runs
every cell once, in a fresh random order drawn from a seed recorded in `session.json` and in
`summary.md`.

```bash
bash experiments/s5b-mgpu/bin/perf.sh P2 reference      # BACKGROUND; SF100 CPU answers, about 10-30 min
bash experiments/s5b-mgpu/bin/perf.sh P2 plan           # runs and estimate; give the user the estimate
bash experiments/s5b-mgpu/bin/perf.sh P2 run            # BACKGROUND; then a confirming session if anything is flagged
bash experiments/s5b-mgpu/bin/perf.sh P2 summarize      # -> results/P2/
```

The warm pass logs at **trace** level, which is the only level that logs per-operator task output
(`operator_produced`, Appendix C). From it come the pair-task count, hash-join time, CONCAT time
and time to the first pair. Trace logging slows the warm run, so these numbers compare policies
with each other; they are not latencies. Latency comes only from the hot passes at info level.

Metrics per cell and query (`queries.parquet`): median hot latency and hot-run spread; the per-join
`build_fold` decisions with rows, bytes and bytes per row (`build_folds.parquet`); pair tasks,
hash-join and CONCAT time, time to the first pair (warm run); reservation shortfalls; OOM retries;
downgrade bytes; CPU match; and whether the logged settings agree with the cell. Peak GPU memory
is per process (`sessions.parquet`).

A (cell, query) is flagged when it is more than 10% slower than `never`, or has more OOM retries
or downgrade bytes than `never`; `run` then confirms the flagged cells, their `never` partners and
the flagged queries in one more session (`confirm_1gpu`, seed + 1), and the decision uses the
confirmed values.

`summary.md` applies the spec's decision criteria in order: (1) correctness equal across policies
(every run completes from pinned tables, matches the CPU, and logged its cell's settings); (2)
disqualify a policy that raises OOM retries or downgrade bytes over `never` on any query; (3)
lowest geomean latency across E1 and E2, no query more than 10% slower than `never`; (4) beats
`never` by more than 2% in geomean in both E1 and E2; (5) otherwise `never`. Criterion 3 shows the
worst query's hot-run spread and criterion 4 the range of the per-pass geomeans. It also breaks
fold decisions out by build width (bytes per row).

**STOP: report to the user and wait for approval.** Report the decision and its numbers per
criterion with their noise, per-policy geomeans for E1 and E2, the worst queries, flagged and
confirmed pairs, disqualifications, and the fold counts by width. **If the decision is not
`never`, ask the user to confirm P3** with the default cells below.

---

## P3 (conditional). Re-check a non-`never` winner at SF1000 on 4 GPUs, tip (about 3-5 h)

Run only if P2's decision is not `never`, and only after the user confirms it at the P2 stop. The
cells: the P2 winner against `never`, at `concat_batch_bytes = H/8` (H of the 4-GPU tip config),
for both E1 and E2, with Dynamic Filters on: 4 cells x 22 queries in one session on host-pinned
SF1000 tables.

```bash
bash experiments/s5b-mgpu/bin/perf.sh P3 plan --policies never,<winner>
bash experiments/s5b-mgpu/bin/perf.sh P3 run --policies never,<winner>          # BACKGROUND
bash experiments/s5b-mgpu/bin/perf.sh P3 summarize --policies never,<winner>    # -> results/P3/
```

It reuses the SF1000 CPU answers from P1. Pass bar: the same decision criteria as P2, with the
same confirmation.

**STOP: report to the user and wait for approval.**

---

## Cleanup (only on the user's go)

Each part asks for `yes` and runs only when the user names it, in this order:

```bash
bash experiments/s5b-mgpu/bin/cleanup.sh work            # delete raw output under S5B_WORK
bash experiments/s5b-mgpu/bin/cleanup.sh worktrees       # remove wt-tip, wt-key, wt-base
bash experiments/s5b-mgpu/bin/cleanup.sh remote-branch   # delete tmp/s5b-mgpu-experiments on kevkrist
```

`work` and `worktrees` delete only directories the kit recorded as created
(`experiments/s5b-mgpu/.created-paths`); `work` also needs the `.s5b-work` sentinel the kit wrote,
and neither deletes a directory that is or contains `/`, `$HOME`, `S5B_HOME`, this checkout, the
results or any TPC-H data. A `S5B_WORK` that existed before the kit ran is never deleted. The TPC-H
data is never deleted by these scripts.

---

## Troubleshooting

- **`refusing to start: something is still running`:** an engine or a step is running. If it is
  your own step (`steps.sh status`), wait for its notification. Otherwise do not kill it; STOP and
  ask the user.
- **A step exits 124:** its hard limit fired and `timeout` killed it. Run `summarize` (the verdict
  says INCOMPLETE), then STOP and report; raise limits (`S5B_STEP_TIMEOUT=<s>`) only with the
  user's approval.
- **A session ends `stalled` or `timeout`:** a query made no progress for the stall limit (P1/P3
  3600 s, P2 1800 s) or the session hit its limit. The next `run` reruns it from scratch; report
  first.
- **LOAD fails right after a previous process** (pool reservation not yet released by the
  driver): rerun the same command; finished units are skipped.
- **A run step exits nonzero:** it has recorded the failure; continue to `summarize`, then STOP
  and report. Rerun failed units only when the user asks (`--retry-failed`).
- **Disk full:** STOP. Raw output is under `S5B_WORK`; ask before deleting anything.

---

## Appendix A. Results tables

Every table is Parquet (zstd), written through `lib/schemas.py`, which refuses a column a table
does not declare and a column two parts of a row both supply. Generated from `lib/schemas.py`:

<!-- BEGIN GENERATED tables by lib/schemas.py; do not edit -->
- **`settings.parquet`** (S0, `S0.settings`): settings a loaded Sirius resolved, per build and GPU count (`probe`). `build` string, `commit` string, `gpus` int32, `name` string, `value` string.
- **`data.parquet`** (S0, `S0.data`): the TPC-H Parquet datasets, from their footers. `sf` int32, `path` string, `tables` int32, `files` int32, `bytes` int64 (Parquet bytes on disk), `uncompressed_bytes` int64 (column-chunk bytes before Parquet compression), `lineitem_rows` int64, `ok` bool, `problem` string.
- **`queries.parquet`** (C2, C4, C5, `C.queries`): one row per label and query (each query is its own process). `stage` string, `label` string (run label, for example `tip_4gpu`), `build` string (tip, key or base), `commit` string, `gpus` int32, `sf` int32, `settings` string (the SET statements of the label), `query` string (q1..q22), `returncode` int32, `result_match` bool (GPU result equals the CPU result), `result_order` string (ordered, or multiset when only the row order differs), `gpu_fallback` bool, `plan_fallback` bool, `gpu_error` string, `key_lines` int32, `constructions_started` int32, `publications` int32, `bloom_transfers_peer` int32, `bloom_transfers_host` int32, `multi_partition_joins` int32, `oom_retries` int32, `reschedules` int32, `downgrade_bytes` int64, `reservation_shortfalls` int32, `reservation_failures` int32, `replica_problems` int32, `severe_lines` int32, `settings_problems` string (logged settings that disagree with the label), `gpu_peak_bytes` int64 (peak GPU pool bytes of the process).
- **`keys.parquet`** (C2, C4, C5, `C.keys`): one row per Key Outcome. `stage` string, `label` string, `build` string, `gpus` int32, `sf` int32, `query` string, `sig` string (join signature), `occ` int32 (occurrence of the signature in the plan), `join` string (operator id), `condition` int32, `build_ordinal` int32, `rows` int64, `valid` string, `kind` string (none, bloom, small_in_list or hash_in_list), `size` int64, `zone_map` bool (logged as 0 or 1), `pushed` int32, `reason` string.
- **`key_diff.parquet`** (C2, `C.key_diff`): one row per Key Outcome compared between the baseline and the tip. `stage` string, `base_label` string, `candidate_label` string, `query` string, `sig` string, `occ` int32, `condition` int32, `status` string (equal, stronger, weaker, extra, missing or duplicate), `zone_map` string (equal, lost or gained), `base_kind` string, `candidate_kind` string, `base_size` int64, `candidate_size` int64, `base_reason` string, `candidate_reason` string, `declared_delta` bool (the candidate reason is a declared delta), `failed` bool (fails the parity script), `strict_failed` bool (fails the C2 bar (declared deltas included)).
- **`bloom_transfers.parquet`** (C2, C4, C5, `C.bloom_transfers`): one row per Bloom Filter reduction. `stage` string, `label` string, `query` string, `root_gpu` int32, `keys` int32, `bytes` int64, `transport` string (peer or host_staging), `sources` int32 (contributing GPUs), `replica_gpus` int32 (GPUs that received a Replica).
- **`publications.parquet`** (C2, C4, C5, `C.publications`): one row per Publication Session that constructed filters. `stage` string, `label` string, `query` string, `host_staged` bool, `filters_pushed` int32 (Dynamic Filters pushed; 0 when nothing was published), `pushed_to` int32 (the count the line reports after `pushed to`), `latency_ms` float64 (publication latency after the final Contribution; 0.0 when unmeasured).
- **`join_strategies.parquet`** (C2, C4, C5, `C.join_strategies`): one row per hash join partition strategy. `stage` string, `label` string, `query` string, `join_id` int32, `partitions` int32, `join_gpus` int32 (GPUs the join saw), `build_bytes` int64, `build_rows` int64 (NULL when unmeasured), `table_bytes` int64, `join_type` string, `mode` string (STANDARD, MIXED_JOIN or BUILD_PROBE), `broadcast` bool.
- **`build_folds.parquet`** (C2, C4, C5, `C.build_folds`): one row per build fold decision. `stage` string, `label` string, `query` string, `sig` string (join signature), `join_id` string (operator id), `fold_policy` string (the `build_fold_policy` the join read), `mode` string (STANDARD or MIXED_JOIN), `rows` int64 (measured build rows; NULL when unmeasured), `resident_bytes` int64, `concat_batch_bytes` int64 (the value the join read), `publishes_filters` bool, `fold` bool, `bytes_per_row` float64 (resident_bytes / rows).
- **`sessions.parquet`** (P1, P2, P3, `P.sessions`): one row per DuckDB process. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `seed` int64 (seed of the shuffled cell order), `cells` int32, `queries` int32, `passes` int32 (1 warm + hot passes), `status` string (ok, error, timeout or stalled), `returncode` int32, `wall_s` float64, `setup_s` float64 (LOAD, views and pinning), `gpu_peak_bytes` int64 (peak GPU pool bytes of the process (max over GPUs)), `host_rss_peak_bytes` int64, `mem_available_min_bytes` int64 (lowest MemAvailable seen), `disk_peak_bytes` int64 (largest size of the session directory seen), `pinned_tables` int32, `compressed_tables` int32 (pinned tables the config compressed), `error` string.
- **`gpu_state.parquet`** (P1, P2, P3, `P.gpu_state`): nvidia-smi clocks, temperature and throttle reasons before and after each process. `stage` string, `session` string, `moment` string (before or after), `gpu` int32, `sm_clock_mhz` int32, `mem_clock_mhz` int32, `max_sm_clock_mhz` int32, `temperature_c` int32, `power_w` float64, `pstate` string, `throttle_mask` string (clocks event reasons bit mask), `throttle_reasons` string (decoded names; `gpu_idle` is normal between processes).
- **`runs.parquet`** (P1, P2, P3, `P.runs`): one row per query run. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `pass` int32 (0 is the warm pass), `kind` string (warm or hot), `latency_ms` float64 (DuckDB CLI `.timer` real time), `status` string, `oom_retries` int32, `reschedules` int32, `downgrade_bytes` int64, `reservation_shortfalls` int32, `reservation_failures` int32, `pinned_scans` int32 (scans served from pinned tables), `plan_fallback` bool, `runtime_fallback` bool, `severe_lines` int32.
- **`queries.parquet`** (P1, P2, P3, `P.queries`): one row per session, cell and query. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `status` string (ok unless a run failed, fell back or read from disk), `warm_ms` float64, `median_hot_ms` float64 (the latency metric), `min_hot_ms` float64, `max_hot_ms` float64, `hot_runs` int32, `hot_spread` float64 ((max - min) / median of the hot runs), `result_hash` string, `result_rows` int64, `ref_match` bool (warm result equals the CPU reference), `ref_order` string (ordered, or multiset when only the row order differs), `ref_detail` string, `fallback` bool, `oom_retries` int32, `reschedules` int32, `downgrade_bytes` int64, `reservation_shortfalls` int32, `reservation_failures` int32, `pinned_scans_min` int32 (fewest pinned scans over the runs), `settings_problems` string (logged settings that disagree with the cell), `pair_tasks` int32 (HASH_JOIN tasks (trace warm run)), `hash_join_ms` float64, `concat_ms` float64, `first_pair_s` float64, `folds` int32, `fold_decisions` int32.
- **`build_folds.parquet`** (P1, P2, P3, `P.build_folds`): build fold decisions of the warm runs. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `sig` string (join signature), `join_id` string (operator id), `fold_policy` string (the `build_fold_policy` the join read), `mode` string (STANDARD or MIXED_JOIN), `rows` int64 (measured build rows; NULL when unmeasured), `resident_bytes` int64, `concat_batch_bytes` int64 (the value the join read), `publishes_filters` bool, `fold` bool, `bytes_per_row` float64 (resident_bytes / rows).
- **`operators.parquet`** (P2, P3, `P.operators`): per-operator task output of the trace warm runs. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `operator` string, `operator_id` int32, `tasks` int32, `total_ms` float64, `first_output_s` float64 (first output after the query window opened).
- **`bloom_transfers.parquet`** (P1, P2, P3, `P.bloom_transfers`): Bloom Filter reductions of the warm runs. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `root_gpu` int32, `keys` int32, `bytes` int64, `transport` string (peer or host_staging), `sources` int32 (contributing GPUs), `replica_gpus` int32 (GPUs that received a Replica).
- **`publications.parquet`** (P1, P2, P3, `P.publications`): Publication Sessions of every run (the line is info level). `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `pass` int32 (0 is the warm pass), `kind` string (warm or hot), `host_staged` bool, `filters_pushed` int32 (Dynamic Filters pushed; 0 when nothing was published), `pushed_to` int32 (the count the line reports after `pushed to`), `latency_ms` float64 (publication latency after the final Contribution; 0.0 when unmeasured).
- **`join_strategies.parquet`** (P1, P2, P3, `P.join_strategies`): hash join partition strategies of the warm runs. `stage` string, `session` string (one DuckDB process, for example `tip_4gpu`), `role` string (base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm), `build` string, `commit` string, `gpus` int32, `sf` int32, `label` string (settings cell), `experiment` string (E1 or E2 (NULL in P1)), `concat_divisor` int32 (H divisor; 0 means 0 bytes (NULL in P1)), `policy` string (build_fold_policy (NULL in P1)), `filters` string (Dynamic Filters on or off (NULL in P1)), `settings` string (the SET statements of the cell), `query` string, `join_id` int32, `partitions` int32, `join_gpus` int32 (GPUs the join saw), `build_bytes` int64, `build_rows` int64 (NULL when unmeasured), `table_bytes` int64, `join_type` string, `mode` string (STANDARD, MIXED_JOIN or BUILD_PROBE), `broadcast` bool.
- **`compare.parquet`** (P1, `P1.compare`): base against tip per GPU count and query. `sf` int32, `gpus` int32, `query` string, `base_a_ms` float64 (first base process), `tip_ms` float64, `base_b_ms` float64 (second base process (A/A)), `base_ms` float64 (geometric mean of base_a_ms and base_b_ms), `ratio` float64 (tip_ms / base_ms), `aa_ratio` float64 (base_b_ms / base_a_ms: the noise of this query), `base_confirm_ms` float64, `tip_confirm_ms` float64, `confirmed_ratio` float64, `final_ratio` float64 (confirmed_ratio when confirmed, else ratio), `identical` bool (base and tip results equal), `base_ref_match` bool, `tip_ref_match` bool, `tip_ref_order` string, `base_oom_retries` int32, `tip_oom_retries` int32, `base_downgrade_bytes` int64, `tip_downgrade_bytes` int64.
- **`compare.parquet`** (P2, P3, `P2.compare`): each fold policy against `never` per cell and query. `experiment` string, `concat_divisor` int32, `filters` string, `policy` string, `query` string, `never_ms` float64, `median_ms` float64, `ratio` float64, `noise` float64 (larger hot-run spread of the two), `same_result_as_never` bool, `ref_match` bool, `oom_over_never` int64, `downgrade_over_never` int64, `folds` int32, `flagged` bool (over a bar before confirmation), `confirmed` bool (a confirming re-run exists), `confirmed_ratio` float64, `confirmed_oom_over_never` int64, `confirmed_downgrade_over_never` int64, `final_ratio` float64, `final_oom_over_never` int64, `final_downgrade_over_never` int64.
- **`reference_sf<N>.parquet`** (P1, P2, `P.reference`): the CPU reference answers. `sf` int32, `query` string, `rows` int64, `ref_hash` string, `cpu_seconds` float64, `spill_peak_bytes` int64 (largest DuckDB temp directory seen), `duckdb_commit` string.
<!-- END GENERATED tables -->

Also: `summary.md` and `excerpts.txt` (or `excerpts/<label>.txt`) per stage; C1 is text only
(`summary.txt`, `catch2_totals.txt`, `skips_and_failures.txt`, `console_tail.txt` per step).

## Appendix B. What the metrics are, and what Sirius cannot provide

- **Latency** is DuckDB CLI `.timer` wall time per statement (plan + execute + result handling) in
  a hot run with `.mode trash`, in a process that has already run the query's warm pass.
- **Peak GPU memory** is the `gpu_pool` line's `peak=`, logged at each query's start and end. The
  peak is never reset, so it is the high-water mark of the process; the kit reports it per process.
- **Downgrade bytes** at info level are the sum of the `downgrade_monitor` and `downgrade_reserve`
  lines; at debug and trace level, the per-request `downgrade_request` lines.
- **Reservation refusals** are not counted by Sirius. A refused non-blocking reservation is not
  logged. The proxy here is `reservation_shortfalls`: the info `downgrade_reserve` lines, one per
  reservation that had to downgrade first. `reservation_failures` counts their `partial`/`failed`
  outcomes and the `reservation_failed` errors.
- **OOM retries** are the warning `reschedule` lines whose reason starts with `OOM`.
- **Pair-task count, hash-join time, CONCAT time, time to first pair** exist only at trace level
  (and in Quent telemetry or nsys NVTX ranges, not used here). Time to the first pair is the first
  HASH_JOIN task output after the query window opened (`window` begin). They come from the warm run
  with trace logging, so they are relative measures.
- **Dynamic Filter counters** (`dynamic_filter_stats`) are not exposed outside C++ tests; the
  `publication` and `bloom_finish` lines stand in for the publication latency and the HOST-staging
  count.
- **IN-list and Zone Map Replica copy path** (peer or HOST) is not logged; only Bloom Filter
  reductions report it.
- **Pinned scans** are the `pinned_scan` lines; a P-stage run without one read from disk and does
  not count.

## Appendix C. Log lines and console markers

Generated from `lib/formats.py`, which `common.py`, `schemas.py` and the parity script all read:

<!-- BEGIN GENERATED formats by lib/schemas.py; do not edit -->
| name | level | source | what it is |
|---|---|---|---|
| `build_fold` | debug | `src/op/partition/consumer_partition_policy.cpp:57` | one line per build fold decision |
| `bloom_finish` | debug | `src/cuda/sirius_dynamic_bloom_filter.cu:1274` | one Bloom Filter reduction and its Replicas, with the transfer path |
| `publication` | info | `src/op/dynamic_filter/dynamic_filter_publisher.cpp:642` | one per Publication Session that constructed filters |
| `construction_started` | info | `src/op/dynamic_filter/dynamic_filter_publisher.cpp:951` | one per Construction; counted |
| `replica_queued` | debug | `src/cuda/sirius_dynamic_bloom_filter.cu:560 (and the IN-list filters)` | one Replica copy; counted |
| `zone_replicated` | debug | `src/op/dynamic_filter/sirius_dynamic_filter.cpp:427` | one Zone Map Replica copy; counted |
| `join_strategy` | debug | `src/op/sirius_physical_hash_join.cpp:995` | one per hash join partition strategy |
| `reschedule` | warning | `src/pipeline/gpu_pipeline_executor.cpp:497` | one task retry; an OOM retry when the reason starts with OOM |
| `retries_exceeded` | error | `src/pipeline/gpu_pipeline_executor.cpp:477` | a task that gave up retrying |
| `downgrade_request` | debug | `src/downgrade/downgrade_executor.cpp:542` | every downgrade request (debug and trace logs) |
| `downgrade_monitor` | info | `src/downgrade/downgrade_executor.cpp:526` | downgrades by the memory monitor (info logs) |
| `downgrade_reserve` | info | `src/pipeline/gpu_pipeline_executor.cpp:341` | one reservation that had to downgrade first (a reservation shortfall) |
| `reservation_failed` | error | `src/pipeline/gpu_pipeline_executor.cpp:251,353` | a reservation failure |
| `gpu_pool` | info | `src/sirius_context.cpp:276` | the process-lifetime peak of one GPU's pool, logged at each query's start and end |
| `window` | info | `src/sirius_context.cpp:742` | an execution window opening or closing; a query run is ok only with an `end ... ok` |
| `gpu_completed` | info | `src/transparent/physical_sirius_execution.cpp:527` | a transparently executed query finished (also after a runtime fallback) |
| `plan_fallback` | info | `src/sirius_context.cpp:1869-2051` | the planner kept the query on the CPU |
| `runtime_fallback` | warning | `src/transparent/physical_sirius_execution.cpp:514, src/sirius_extension.cpp:692` | the GPU failed mid-query and DuckDB re-ran it on the CPU |
| `pinned_scan` | info | `src/scan_manager/sirius_scan_manager.cpp:4062` | a scan served from a pinned table; a P-stage query run without one read from disk |
| `pin_compressing` | info | `src/sirius_extension.cpp:1368` | a pinned table the machine config compresses |
| `replica_problem` | warning | `src/cuda/dynamic_filter_replica_transfer.cu:79 (and the filters' skip lines)` | a Replica that was not made |
| `operator_produced` | trace | `src/pipeline/gpu_pipeline_task.cpp:161` | one operator task's output; the pair-task, hash-join and CONCAT measures |
| `severe` | error | `any` | any error or critical line; counted and excerpted |
| `dynamic_filter_key` | debug | `src/op/dynamic_filter/dynamic_filter_key_outcome.cpp (debug)` | the Key Outcome line: `v`, `sig`, `occ`, `join`, `condition`, `build_ordinal`, `rows`, `valid`, `kind`, `size`, `zone_map`, `pushed`, `reason` |
| `fallback_banner` | stdout | `print_cpu_fallback_banner()` | `Error in Sirius GPU execution, fallback to DuckDB` |
| `timer` | stdout | `duckdb tools/shell/shell.cpp:225` | `.timer on 6` output, one per SQL statement |

Samples (one per format, as Sirius prints it):

```
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] [build_fold] v=1 sig=00ab00cd00ef0011 join=17 policy=bounded mode=STANDARD rows=1000 resident_bytes=64000 concat_batch_bytes=1024 publishes_filters=true fold=true
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] [accumulated_bloom_builder::finish] root GPU 2 reduced and replicated 3 key(s), 4096 bytes in all, through HOST staging: 4 contributing source(s), 3 target(s).
[2026-11-13 10:00:00.200] [info] [p.cpp:642] [dynamic_filter_publication_session] constructed filters published (Bloom through HOST staging): 5 filter(s) pushed to 7 target(s), 1.5 ms after the final contribution.
[2026-11-13 10:00:00.050] [info] [p.cpp:951] [dynamic_filter_publication_session] construction started: 2 key(s), 9 batch(es) on 4 GPU(s).
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] [sirius_dynamic_bloom_filter] queued 4096-byte replica GPU 0 -> GPU 3.
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] [sirius_dynamic_zone_map_filter] replicated 2 zone(s) GPU 1 -> GPU 0.
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] sirius_physical_hash_join id 13 partition strategy: 1 partitions (4 GPUs), build side 10 bytes, 0 rows (rows-unmeasured), estimated hash table 18446744073709551615 bytes. Join Type: RIGHT_SEMI. Join Mode: MIXED_JOIN  [broadcast]. build_card_est 1 probe_card_est 2 placement x
[2026-11-13 10:00:01.000] [warning] [e.cpp:497] GPU Pipeline Executor: reschedule (retry 1/100) for task 5 (original task 3), resuming from operator index 2: OOM at operator HASH_JOIN
[2026-11-13 10:00:01.000] [error] [e.cpp:477] GPU Pipeline Executor: task 5 (original task 3) exceeded 100 retries at operator HASH_JOIN
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] [downgrade] [GPU:0] request done: 3 batches, 2048 bytes (target 1000, planned 2048) in 1.00 ms
[2026-11-13 10:00:01.000] [info] [d.cpp:526] [downgrade][monitor] [GPU:0] request done: 2 batches, 1000 bytes in 1.00 ms (1.0 MB/s), satisfied=true | to_host: 2/1000 batches/bytes, to_disk: 0/0 batches/bytes
[2026-11-13 10:00:01.000] [info] [e.cpp:341] [downgrade][reserve] pipeline 3 task 9: needed 500 bytes (shortfall 200), downgrade freed 300 bytes, reservation partial
[2026-11-13 10:00:01.000] [error] [e.cpp:251] GPU Pipeline Executor: Failed to acquire memory reservation for task 9
[2026-11-13 10:00:00.000] [info] [c.cpp:276] [gpu_pool] GPU:1 QueryEnd instance=0x1 connection=1 window=2 query=3 allocated=10 bytes peak=999 bytes reserved=0 bytes
[2026-11-13 10:00:02.000] [info] [c.cpp:742] [window] end instance=0x1 connection=1 window=2 query=3 outcome=ok
[2026-11-13 10:00:02.000] [info] [s.cpp:527] Transparent GPU execution: query completed
[2026-11-13 10:00:00.000] [info] [c.cpp:2041] Transparent execution fallback (unsupported): window function
[2026-11-13 10:00:01.000] [warning] [t.cpp:514] Transparent execution: GPU execution failed at runtime; falling back to DuckDB CPU within the same transaction. GPU error: validity row_offset (3) not byte-aligned
[2026-11-13 10:00:00.010] [info] [m.cpp:4062] [sirius_scan_manager] assigned pinned entry 'lineitem' to operator '3'
[2026-11-13 10:00:00.000] [info] [e.cpp:1368] [pin_table] 'lineitem' tier=host: compressing with plan for 16 column(s)
[2026-11-13 10:00:00.300] [warning] [r.cu:79] bloom replica GPU 0 -> GPU 2 unavailable: out of memory. That GPU will skip this optional filter.
[2026-11-13 10:00:00.500] [trace] [t.cpp:161] [GPU:0] Pipeline 4: operator HASH_JOIN (id=12) task=77 produced 1 batches, num rows: 10  , size: 80 bytes (0.00 MB). execution time: 2.50 ms, peak allocated: 100 bytes (0.00 MB)
[2026-11-13 10:00:00.000] [error] [x.cpp:1] something failed
[2026-11-13 10:00:00.100] [debug] [x.cpp:1] [dynamic_filter_key] v=1 sig=6b85214e8d4b623a occ=0 join=17 condition=0 build_ordinal=0 rows=100 valid=90 kind=bloom size=4096 zone_map=1 pushed=2 reason=-
Run Time (s): real 1.250000 user 0.100000 sys 0.000000
```
<!-- END GENERATED formats -->
