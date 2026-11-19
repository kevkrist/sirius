# dfperf recovery index

Written 2026-11-19 after the EC2 instance-storage wipe of `/opt/dlami/nvme/dfperf` (called R below).
This directory (`/home/ubuntu/dfperf-durable`, called D) is the durable, text-only copy on the root disk.
A copy of this file is at `R/notes/RECOVERY.md`.

## What the campaign is

A multi-day performance campaign on the Sirius dynamic-filter (DF) PR stack #2569 -> #2570 (tip `ae939824`, merge-base
`c4fa3638`), measured on TPC-H SF1000 with 4 GPUs. Phases: (1) map the code (7 notes), (2) measure baseline, profile
(Quent + nsys) and the base reference, plus Bloom and lifecycle microbenchmarks, (3) verify and synthesize into
`notes/OPPORTUNITIES.md`, (4) prototype each opportunity in its own worktree and have an independent verifier re-measure
it. Reference totals (median of 6 hot runs): base 15225 ms, tip 15372 ms, tip with DF off 16627 ms; the stack is
147 ms (1.0%) slower than base. User decisions of 2026-11-18 (top of OPPORTUNITIES.md) redirected the work: eager
probe scheduling is the real goal (S6, issue #1825), memory transfers are in scope, and transfer changes go in a
separate PR.

Everything was recovered by replaying the tool calls in the agent transcripts under
`/home/ubuntu/.claude/projects/-home-ubuntu-sirius/90fe0da0-364a-41dc-b0f4-acbde346fc58/subagents/workflows/`.

## Layout

| Durable (D) | Restored (R) | Content |
|---|---|---|
| `notes/` | `notes/` | 31 notes (identical copies) |
| `scripts/runs/<group>/...` | `runs/<group>/...` | run and analysis scripts, case lists |
| `scripts/mblife/` | `mblife/` | lifecycle microbench scripts |
| `scripts/setup.sh` | `setup.sh` | clone + data generation + tip build |
| `patches/<branch>.patch` | `wt-*` worktrees of `R/repo` | `git diff pr2570` per branch |
| `patches/<branch>.md` | - | commits, switches, build/run, gaps |
| `patches/<branch>.commits.mbox` | - | per-commit split (6 branches; the eager-opt2 mbox holds only its 3 commits over `proto/op1-minimal`) |
| `patches/*.over-*.patch`, `*.on-*.patch` | - | delta over a base branch only |

Status words: EXACT = replayed completely; PARTIAL = parts missing (marked in the file); RECONSTRUCTED = inferred;
LOST = gone.

## Verified on disk (indexer, 2026-11-19)

- D is 3.1 MB. `diff -r` of D/notes vs R/notes, D/scripts/runs vs R/runs, D/scripts/mblife vs R/mblife: no difference.
- All 12 full patches pass `git apply --check` on a clean pr2570 (`ae939824`) checkout.
- All 12 worktrees are clean, on their branch, with the commits listed below. Nothing is pushed.
- Nothing was compiled or run after recovery. Submodules are not initialised in the worktrees.

## Notes

| File | Status | Remark |
|---|---|---|
| construction, kernels, probe_publish, concurrency, pr_split, tooling `.md` | EXACT | code maps |
| HYPOTHESES.md, VERIFY_methodology.md, VERIFY_transfer.md | EXACT | |
| OPPORTUNITIES.md | EXACT | synthesis + USER DECISIONS header |
| ISSUE_1818.md, ISSUE_1825_S6.md | EXACT | re-fetched with gh |
| BASELINE_SUITE.md, PROFILE_SUITE.md | EXACT | |
| BASE_REFERENCE.md | PARTIAL | lines 1-200 exact; coverage-diff appendix (q4..q22): 57 lines from earlier runs (`RECOVERED` tags), 42 lines `LOST` (q11 rows, consumer / operator-task lines of q10-q22) |
| MB_BLOOM.md, MB_LIFECYCLE.md | EXACT | |
| MB_TRANSFER.md | PARTIAL | section 7: 6 contention values (bg=p2p01+host02) marked `[LOST: not in transcript]` |
| PROTO_OP1A.md, VERIFY_PROTO_OP1A.md | EXACT | verify note unfinished (second block "queued") |
| PROTO_OP1B.md, VERIFY_PROTO_OP1B.md | EXACT | |
| PROTO_OP2.md, VERIFY_PROTO_OP2.md | EXACT | |
| PROTO_OP4.md, VERIFY_PROTO_OP4.md | EXACT | |
| PROTO_SGB.md | EXACT | |
| VERIFY_PROTO_SGB.md | EXACT, unfinished | section 1.2 still holds `SECOND_WAVE_PLACEHOLDER` |
| EAGER_SCOUT.md | EXACT | |
| PROTO_EAGER.md | EXACT, unfinished | section 6 (throttle / option 2) never written |
| PROTO_XFER_HOST.md | EXACT | design + plan only |
| PROTO_XFER_D2D.md, VERIFY_PROTO_EAGER*.md | never existed | |

## Scripts

| Path (under `runs/` unless noted) | Status |
|---|---|
| `verify/bin`, `verify_transfer/` | EXACT |
| `baseline/bin`, `profile/bin`, `base/` (incl. `build.sh`) | EXACT |
| `baseline`, `base` derived tables (`timing_table.md`, `df_compact.md`, `nsys_compare.md`) | EXACT |
| `base/logpass/coverage_diff`, `profile/{combined,nsys_out/tables,quent/table,quent/tip_vs_off}` | PARTIAL (`.PARTIAL*` names) |
| `mb_bloom/`, `mblife/` (top level of R) | EXACT |
| `op1a/bin`, `vop1a/bin` | EXACT |
| `op1b/bin`, `vop1b/bin` | EXACT, except `vop1b/bin/edge.py` RECONSTRUCTED (high confidence) |
| `op2/bin`, `vop2/bin` | EXACT |
| `op4/bin`, `op4/mb/c_set1.txt`, `vop4/bin` | EXACT |
| `sgb/bin`, `vsgb/bin` | EXACT |
| `eager/bin`, `eager_scout/bin` | EXACT |
| `mb_xfer/bin` | EXACT |
| `xfer_d2d/bin` | EXACT, except `todo.txt` RECONSTRUCTED, `abba_table.py` derived by sed; `nsys_df.py` copied from `profile/bin` by the indexer |
| `xfer_host/bin`, `xfer_host/queue.txt` | EXACT |
| `setup.sh` (top level of R) | as found in R after the re-setup |

## Prototype branches (all based on pr2570 `ae939824`)

Hashes are the recovered local commits; notes cite the ORIGINAL hashes (mapping in each `patches/<branch>.md`).

| Branch / worktree | Head | Code | How far it got | Unfinished |
|---|---|---|---|---|
| `mb/bloom` / `wt-mb-bloom` | 80958cc8 | EXACT | built, measured, written up | - |
| `mb/lifecycle` / `wt-mb-life` | b31ca131 | EXACT | built, measured, written up | - |
| `mb/transfer` / `wt-mb-xfer` | 4eb4e6d5 | EXACT | measured, written up | `cont2`/`bgleg` follow-up never compiled or run |
| `proto/op1-continuation` / `wt-op1a` | 61eb1453 (6) | EXACT | measured: -167 ms (-1.09%), all host-bounce time, zero on P2P pair; drop | verifier second block (q10/q9/q5/q6 + 13 more) never ran; no final return |
| `proto/op1-minimal` / `wt-op1b` | 37850ed0 (2) | EXACT | measured -168 ms on 13 queries; verified; drop for the stack | - |
| `proto/op2-dead-filters` / `wt-op2` | 41d36d9d (3) | EXACT | measured + verified; shadow rule: land after hardening; LATCH needs more; ENDPOINT, OP3 drop | - |
| `proto/op4-filter-kind` / `wt-op4` | 7f1e2f6f (2) | EXACT | measured + verified; OP4 drop; OP5 (8 bits/key) gain is build-site time only | - |
| `proto/single-gpu-build` / `wt-sgb` | 28ad6e92 (3) | EXACT | measured; verifier: timing and correctness confirmed, value weakened | verifier second wave (v2, c1, c2, late_body_a/b) never ran |
| `proto/eager-probe` / `wt-eager` | f05cfd12 (1) | EXACT | option 1 measured on 22 queries: -295 ms (-1.9%), results identical | no verifier started |
| `proto/eager-opt2` / `wt-eager2` | eb221343 (5, on op1-minimal) | EXACT | built, smoke tests pass; batch b1 3 of 9 queries (q3+q8+q21: opt1 -148, K=1 +4, K=2 -183, opt2 -107 ms) | rest of b1, all of c1, verdict on throttle and option 2, verification |
| `proto/xfer-d2d` / `wt-xfer-d2d` | c15b053e (2) | EXACT (was uncommitted) | design written in code | never compiled, nothing measured, no note |
| `proto/xfer-host` / `wt-xfer-host` | 336915e1 (2) | EXACT | design written and committed, 16 jobs planned | never compiled, nothing measured |

Cross-branch dependency: op1a, sgb, xfer-d2d and xfer-host carry a cherry-pick of `mb/lifecycle`; op4 carries
`mb/bloom`; eager-opt2 sits on `proto/op1-minimal`.

## Lost for good

- All raw measurement data: run directories, logs, telemetry, nsys traces, Quent JSON, summary JSON, build output.
  The numbers live in the notes.
- 42 lines of the BASE_REFERENCE coverage-diff appendix and 6 contention values in MB_TRANSFER section 7.
- Generated tables that were only files (`df_tables.md`, `combined.json`, `runs/op1b/out/*.md`, `runs/eager/out/*`):
  regenerate by re-running the passes with the recovered scripts.
- Ad-hoc inline analysis snippets (never files); their text is still in the transcripts.
- Queue state of the three agents that were still running (op1a verifier, sgb verifier, eager proto).

Not lost but not recreated: `R/wt-base` (worktree at `c4fa3638`, built by `runs/base/build.sh`), `R/machine.lock`
(created on first use), `R/telemetry/`, builds of every prototype worktree.

## Restore procedure (after another wipe)

```bash
R=/opt/dlami/nvme/dfperf; D=/home/ubuntu/dfperf-durable
mkdir -p $R/{logs,tmp,cache,data}
cp $D/scripts/setup.sh $R/setup.sh
bash $R/setup.sh > $R/logs/setup.log 2>&1      # clone, branches pr2569/pr2570, SF1000 data, tip build (hours)
git -C $R/repo rev-parse pr2570                 # must be ae939824, else the patches may not apply

mkdir -p $R/notes && cp -a $D/notes/. $R/notes/ && cp $D/README.md $R/notes/RECOVERY.md
cp -a $D/scripts/runs $R/runs && cp -a $D/scripts/mblife $R/mblife

wt() {  # branch worktree-dir patch-name
  git -C $R/repo worktree add -b "$1" "$R/$2" pr2570
  if [ -f "$D/patches/$3.commits.mbox" ]; then git -C "$R/$2" am "$D/patches/$3.commits.mbox"
  else git -C "$R/$2" apply --index "$D/patches/$3.patch" && git -C "$R/$2" commit -qm "recovered prototype: $1"; fi
}
wt mb/bloom               wt-mb-bloom  mb-bloom
wt mb/lifecycle           wt-mb-life   mb-lifecycle
wt mb/transfer            wt-mb-xfer   mb-transfer
wt proto/op1-continuation wt-op1a      proto-op1-continuation
wt proto/op1-minimal      wt-op1b      proto-op1-minimal
wt proto/op2-dead-filters wt-op2       proto-op2-dead-filters
wt proto/op4-filter-kind  wt-op4       proto-op4-filter-kind
wt proto/single-gpu-build wt-sgb       proto-single-gpu-build
wt proto/eager-probe      wt-eager     proto-eager-probe
# eager-opt2 is stacked on op1-minimal; its mbox holds only the 3 commits on top
git -C $R/repo worktree add -b proto/eager-opt2 $R/wt-eager2 proto/op1-minimal
git -C $R/wt-eager2 am $D/patches/proto-eager-opt2.commits.mbox
wt proto/xfer-d2d         wt-xfer-d2d  proto-xfer-d2d
wt proto/xfer-host        wt-xfer-host proto-xfer-host
```

Then, per worktree that will be built: `git submodule update --init --recursive`, and build with that group's
`runs/<group>/bin/build.sh`. For base comparisons recreate `R/wt-base` with `runs/base/build.sh`. Expect compile
errors on the first build of `proto/xfer-d2d` and `proto/xfer-host`. If the mbox does not apply (`git am` fails),
run `git am --abort` and use the `.patch` path. Never push these branches.

Keep D current: after any new note, script or commit under R, copy the text back to D (no data, no binaries).
