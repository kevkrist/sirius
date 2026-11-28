# IMPL_TRANSFER_PR: production implementation of the transfer PR [xfer-PR]

Branch `prod/xfer` (from `pr2570` ae939824), worktree `$R/wt-prod-xfer`. Design: DESIGN_TRANSFER_PR.md (amended by step 0).
Build: `$R/tmp/xfer_build.sh` (flock + pixi run make, log `$R/logs/build_prod_xfer.log`).

## 1. What was built (commits on `prod/xfer`, oldest first)

| Commit | Content |
|---|---|
| `feat(memory): group GPUs into islands of mutual pool access` | `src/memory/peer_islands.{hpp,cpp}`: `islands_from_access` (pure, greedy clique cover) + `peer_islands(spaces)` (from `try_grant_pool_access`). Test `test/cpp/memory/test_peer_islands.cpp`. |
| `feat(memory): add a host OR kernel and shared worker threads` | `src/memory/host_or.{hpp,cpp}`: `host_or` (AVX-512 non-temporal, 8-byte fallback, runtime dispatch), concept `host_or_executor`, `inline_host_or_executor`, `host_or_workers` (process-wide, `shared()`, T = clamp(cores/4, 1, 8), claim-based jobs: the submitter always works, never waits for a helper). Test `test/cpp/memory/test_host_or.cpp`. |
| `feat(dynamic-filter): plan the roles of a multi-GPU array union` | `src/op/dynamic_filter/detail/union_plan.{hpp,cpp}`: leader / sources / others per island, `split`, `inputs`, `crosses_host`, `uploads`, `host_slots`, `merge_slots`. Exhaustive test over every grouping of <= 8 GPUs x masks x roots (8.5 M plans). |
| `feat(dynamic-filter): add unite, the multi-GPU array union` | `src/op/dynamic_filter/detail/union_transfer.hpp` (interface: `union_request`, `union_report`, `union_or_runner`, `unite`) + `src/cuda/union_transfer.cu` (executor: stage A merge, B cross with CPU OR / pass-through, C fan-out; degradation; one join + one release). Takes the OR kernel from the Bloom file. |
| `feat(dynamic-filter)!: publish accumulated Bloom filters through unite` | `finish_impl` = validate + build request + `unite` + `make_filters`. Deleted: `chunk_pipeline`, the tip serial HOST loop, `force_host`, `publication_transport`, `workspace_observation`. New: `publication_route {NONE, PEER, PEER_AND_HOST, HOST}` + `degraded`, counter `accumulation_publications_degraded`, builder stores `peer_islands` at `try_create`, `sirius_memory_reservation_manager` keeps `host_or_workers::shared()` alive. Tests updated + added (section 3). |
| `docs(dynamic-filter): describe the Bloom transfer through unite` | `docs/super-sirius/dynamic-filters.md` transport + workspace paragraphs, limits bullet. |

No env switch, no mode, no size threshold. Source diff vs `pr2570`: see `git diff --stat pr2570 prod/xfer`.

## 2. Deviations from DESIGN_TRANSFER_PR.md (each with its reason)

1. **Helper threads reset their CPU affinity** (`use_process_cpus`, not in the design). Found in the engine: the thread that constructs the
   memory manager is bound to 1 CPU, new threads inherit that, so all helpers shared one core: q21 finish 130-150 ms (host OR 119 ms) instead
   of 74. With the fix 73-75 ms. Unit test added. The prototype did not see it (its pool was created from a task thread).
2. **Final wait = one `cudaStreamSynchronize` of the publication stream**, which first waits for one completion event per used stream
   (design: host-synchronize every stream). Reason: keeps the tip PEER contract "the publication stream is busy until the publication is
   complete" that an existing test (`late cancellation or drain ...`) relies on; also I10. On failure every stream is still joined one by one.
3. **I7 softened**: a publication that moves nothing (one GPU / no contribution) still waits for the partial streams (zero fill, inserts) as
   the tip did; it submits no copy and allocates no workspace. Publishing arrays whose zero fill is still pending would be a wrong-result risk.
4. **I8 is an always-on check in the builder** (override must divide the builder's own islands, else `accumulation_invariant_error`) instead
   of a debug-only precondition inside `unite`: testable in release builds, O(G^2) host work. `unite` itself trusts `island_ids` (documented).
5. **`unite` is a non-template function** taking `union_or_runner` (the erased `host_or_executor`, built with `union_or_runner::of(executor)`),
   so the CUDA executor is compiled once in the `.cu`; the concept stays at the boundary.
6. **Root island ingress runs on the publication stream** (as the tip PEER pipeline), other leaders lease an ingress stream. Design: one
   leased ingress stream per leader with sources.
7. **`union_report` has `peer_copies`, `host_or_ms`, `host_wait_ms`** in addition to the designed fields; the route is derived from
   `host_inputs` + `peer_copies`. The DEBUG line logs elapsed ms, islands, inputs, OR ms, wait ms, W, pinned bytes and the SUM of device
   scratch (not per leader: `fmt::join` is avoided in this repo).
8. **Helper spin**: a helper polls for the next job for 500 us after its last part (also after the last job of a `unite`), then sleeps.
   Design: "no thread spins while no unite is active". Measured: no spin costs 2-4% (section 4.2); a session API was not worth it.
9. **Commits 4 and 5 of the plan are not "one island first, then the cross stage"**: `unite` was written whole (commit 4, no caller) and the
   switch + deletion of both transports is commit 5. Each commit builds; the tree is green at commit 5.
10. **Not built from the test plan**: copy-count assertion for I10 (scratch / pinned / route for one island are asserted on the 2-GPU fixture);
    `copy_batch` mid-chunk faults (the batch call is issued inside libcudf and is not interceptable; `enqueue_copy_batch` at the Sirius boundary
    is used); a real "stream cannot be joined" case (the harness cannot produce one; the injected final-wait failure is covered).
11. Required reading: the three AgentDocs books and the Core Guidelines were not re-read for this task (context budget); cudf doc style followed.

## 3. Tests

New files: `test_peer_islands.cpp`, `test_host_or.cpp`, `test_dynamic_filter_union_plan.cpp`, `test_dynamic_filter_union_transfer.cpp`
(bit identity over device counts 2/3/4 x real + 3 divided groupings x all contribution masks x every root x 1-2 keys x pool / inline
executor; array shapes 96 B .. 70 MiB with different sizes per key; root scratch refused; other leader lease refused; override that joins
unreachable GPUs rejected; inserts and zero fill still pending at finish; 8 concurrent publications; hidden bench `[union_transfer_bench]`).
Rewritten: `test_dynamic_filter_accumulation_fault.cpp` (2-GPU fault points for both routes, preparation event failure, delay points,
pipeline property replacing "HOST publication submits every target upload", W = 1 / refusal replacing "HOST exhaustion", 13 fault points on the
real 4-GPU islands, "moves nothing"). Changed expectations (design 10.2, Q2): `test_dynamic_filter_accumulation.cpp` (workspace tests, tracked-thread
scratch refusal now publishes degraded), `test_in_task_publication.cpp` (scratch OOM publishes through the host, degraded == 1; refused pinned window
retires partials).

## Checkpoint log (chronological; a fresh session resumes from the last line)
- 11-28 start: worktree created (`git -c protocol.file.allow=always submodule update --init --recursive`), baseline build started.
- commits 1-3 done (17333840 peer_islands, ff23950c host_or + workers, e01a623a union_plan), tests green ([peer_islands],[host_or],[union_plan]).
- WIP (uncommitted in worktree): src/cuda/union_transfer.cu + detail/union_transfer.hpp (unite: merge / cross with CPU OR / fan-out,
  one drain + one release), finish_impl switched to unite, chunk_pipeline + tip HOST loop + force_host + publication_transport deleted,
  publication_route + degraded, counter accumulation_publications_degraded, manager holds host_or_workers::shared().
  Tests: test seam = builder_access::finish(builder, root, stream, mr, {.islands, .inline_or, .report}); acc::separate_gpus.
  Fault test file rewritten (not yet built). TODO: fix remaining test call sites (test_dynamic_filter_accumulation.cpp ~2078-2230,
  test_in_task_publication.cpp 852-960, test_pool_peer_access.cpp, multi_partition integration), new test file
  test_dynamic_filter_union_transfer.cpp (identity over topologies, degradation, concurrency, inserts in flight), docs, gates.
- helper scripts: $R/tmp/xfer_build.sh, xfer_test.sh <bin> <catch args>, xfer_pc.sh (pre-commit), xfer_commit.sh "<msg>".
- commits 4-5 done: 5c34bfd8 (unite module), 3de0c746 (finish switched, both tip transports deleted, tests updated/added).
  Unit gates green at 3de0c746: sirius_unittest "[dynamic_filter]~[cuda_status_fault]~[.],[device_transfer],[publisher],[construction_plan],
  [physical_partition],[build_filter],[in_task_publication],[pool_peer_access],[peer_islands],[host_or]" = 741 cases pass;
  sirius_cuda_fault_test "[cuda_status_fault]~[.]" = 22 cases pass. Pre-commit clean. NEXT: engine gates (22 q identity, ABBA), docs, notes.
- 03:50 FOUND + FIXED: engine finish was 130-150 ms on q21 (host OR 119 ms): the thread that constructs the memory manager is bound
  to 1 CPU, helper threads inherited it. Fix: helpers call use_process_cpus() (affinity of the process main thread) + test. q21 finish 73-75 ms.
- 04:00 commits rewritten by autosquash: 17333840, e80d9298, 4e6b4d1c, 786778a7, 498fe2fc. Hidden bench: sirius_unittest "[union_transfer_bench]".
- 04:05 OR-parts / spin sweep done (hack patch $R/tmp/xfer_sweep_hack.patch, not committed); rule min(T, 2 x Kc) and 500 us spin kept. Binary rebuilt = HEAD.
- NEXT: engine ABBA (runs/xfer_prod, bin/abba_q.sh <tag> tppt|pttp 6 q...; table: bin/table.py e1 e2), 22-query identity, docs, final notes.

## Checkpoint log 2 (main-based delivery, 2026-11-28; user: PR is standalone on origin/main)
- origin/main fetched = a94d5c4b (80 commits after c4fa3638, the stack's base). main needs pixi >= 0.84.2: $R/pixi-new/pixi (0.85.0), used by $R/tmp/wt_build.sh <worktree> [target].
- wt-main (detached origin/main) = baseline build for measurement 1. wt-prod-xfer-main = branch prod/xfer-main.
- prod/xfer WIP committed as ed3a54b8 (bench split, pool_peer_access doc).
- cherry-picked onto main: peer_islands, host_or, union_plan, unite (cmake conflicts only). Switch commit 498fe2fc: conflicts resolved in
  bloom .cu (geometry -> union_shape per key), builder hpp (keep supports/geometry), publisher (published_filters->route/degraded), stats, tests
  (tags [construction] -> [multi_partition] on main). NOT yet built / committed.
- Plan: prod/ctl-stack-2569-on-main, prod/ctl-stack-2570-on-main (stack rebased on origin/main, control for measurement 2), then rebase those onto prod/xfer-main.
- prod/ctl-stack-2569-on-main, prod/ctl-stack-2570-on-main (worktree $R/wt-stack) = apply/2569 / apply/2570 rebased on origin/main a94d5c4b. NOT built yet.
  Conflicts (main drift, not transfer): (1) c4b4e135 sirius_plan_comparison_join.cpp: keep main's RIGHT_SEMI/ANTI kept-build block + signature line;
  (2) 828c353d same file: discovery_runs = build_evidence && spaces (main dropped the LOGICAL_COMPARISON_JOIN test, commit drops the right-family gate);
  (3) 75d55790 retire option: ported to main's typed config (src/config/options/operator.hpp: option deleted, removed(...) entry added; sirius_config.cpp mapping;
  test_config_create / test_config_operator_options / valid_every_key.yaml / configuration.md; test_context indices 22-25 -> 21-24; sip test line);
  (4) 4f1a9575 configuration.md paragraph: stack text; (5) 1d976a61 removal message moved to operator.hpp + operator_options test, test_config.cpp includes.
- prod/xfer-main now = origin/main + 6 picks (1af0e6e1 islands, 62e6dee3 host_or, 8a659907 plan, 4b5b6b45 unite, 18d0a2e8 switch, 96581815 docs) + UNCOMMITTED
  WIP from ed3a54b8 (bench split, pool_peer_access doc). Main adaptations: test_dynamic_filter_union_transfer.cpp uses uniform geometry (geometry_of(blocks, keys);
  "different sizes per key" shapes made equal per key: restore the prod/xfer version when #2569 brings per-key blocks); docs applied to main's paragraphs.
  Build of wt-prod-xfer-main queued (log $R/logs/build_wt-prod-xfer-main.log). git rerere enabled in the repo.
- Run scripts: $R/runs/xfer_main/bin/abba_q.sh <tag> <armA>=<wt> <armB>=<wt> abba|baab <iters> q...; table.py <armA> <armB> <tag...>.
- NEXT: fix compile errors, unit tests ([dynamic_filter],[peer_islands],[host_or],[union_plan],[in_task_publication],[pool_peer_access]; sirius_cuda_fault_test),
  pre-commit, squash WIP into switch commit; rebase prod/ctl-stack-* onto prod/xfer-main -> prod/stack-2569-on-xfer / prod/stack-2570-on-xfer (worktree wt-stack-xfer);
  builds: wt-main (A), wt-prod-xfer-main (B), wt-stack (C = prod/ctl-stack-2570-on-main), wt-stack-xfer (D); measure B vs A and D vs C.
- prod/xfer-main FINAL CANDIDATE = 9038a957 (6 commits on origin/main a94d5c4b). Built (full pixi run make rc=0, tree a57dbf51 + doc-only delta), unit gates green:
  sirius_unittest "[dynamic_filter]~[cuda_status_fault]~[.],[device_transfer],[publisher],[physical_partition],[build_filter],[in_task_publication],[pool_peer_access],[peer_islands],[host_or],[union_plan]"
  = 649 cases pass (4 GPUs); sirius_cuda_fault_test "[cuda_status_fault]~[.]" = 25 cases pass; pre-commit clean.
  Main-only additions: src/cuda/accumulation_cuda.cuh (check_cuda / cuda_event / make_event_on shared by the Bloom .cu and union_transfer.cu; on the stack
  accumulation_partial.cuh now includes it instead of defining them).
- Stack on transfer: prod/stack-2570-on-xfer = 1a9df698 (25 commits grafted on 9038a957; $R/tmp/graft.py, res.py, cont.sh). Textual conflicts: cmake lists (union);
  e4b71389 bloom .cu (take transfer finish, per-key shapes); 7a4ac11e bloom .cu include + create() (lease_partials then islands), accumulation_partial.cuh;
  82fdfe91 accumulation_partial.cuh; 49445de2 publisher (route / bloom_degraded); 2fe817f7 test tags [multi_partition]->[construction] incl. union test;
  966cc597 + 0ceaf75e docs (stack text + transfer paragraphs); 9ef40276 builder hpp try_create doc.
- IN PROGRESS: wt-stack-xfer detached at 89ebb7d5 (2569 head on transfer) + UNCOMMITTED semantic fixes F1 (build_filter_construction route/bloom_degraded, stats prose,
  builder hpp docs, orphan comment, union test = prod/xfer ed3a54b8 version with [multi_partition] tags); building (log build_wt-stack-xfer.log). Then: commit F1 as
  prod/stack-2569-on-xfer head, graft the 11 #2570 commits on it (tree + F1 patch; union test from $R/tmp/union_test_c.cpp once tags are [construction]), F2 at 2570 head
  (stats bullets, check vs prod/xfer-on-shadow), build, tests.
- Measurement 1 queued: runs/xfer_main m1_* (ABBA main vs xfer, q21 3 7 8 10 9 6 2 4) and id1_* (22 q, debug log, 2 iters). Table: bin/table.py main xfer m1.

## Checkpoint log 3 (finishing session, 2026-11-28 afternoon; NVMe was wiped and restored, old worktrees / $R/tmp scripts lost)
- origin/main = f1595eb3 (a94d5c4b + 1 scan commit, no overlap). All five branches rebased onto it, patches byte-identical (index / hunk offsets only):
  prod/xfer-main 8c99539f; prod/ctl-stack-2569-on-main 7abf43af; prod/ctl-stack-2570-on-main 61d61249; prod/stack-2569-on-xfer 91dbc27f;
  prod/stack-2570-on-xfer 67aeb069. Heads before this rebase: prod/old-a94d/* (9038a957, 83d4a8af, 03f77567, cea78cba, 1e81bbd3).
  Repo-local git identity set to the PR author (author + committer).
- Worktrees: $R/wt-main (origin/main, arm A), wt-xfer (B), wt-ctl (C = ctl-stack-2570), wt-stack (D = stack-2570-on-xfer). pixi 0.85.0 re-downloaded
  to $R/pixi-new. Scripts: $R/tmp/wt_build.sh <wt>, wt_test.sh <wt> <binary> <catch args>, wt_submods.sh <wt> (only the submodules the build needs,
  cloned from wt-xfer's local stores; a full recursive update clones starrocks / doris per worktree and takes > 20 min).
- Rebase replay (conflict list): $R/notes/xfer_rebase/replay_rebase.sh + replay.log.
- Run dir $R/runs/xfer_fin (bin/abba_q.sh, one_query.sh, table.py as in runs/xfer_main). DF queries: 2 3 4 5 7 8 9 10 11 12 16 17 18 19 20 21 22; non-DF 1 6 13 14 15.
- NEXT: builds A-D, unit gates on B and D (+ 2569 head), pre-commit on B, measurement (a) B vs A, (b) D vs C, 22-query identity, tables below.

## 5. Rebasing the construction stack (#2569, #2570) over the transfer PR -- reproducible recipe (2026-11-28)

Inputs: `X` = transfer PR head (`prod/xfer-main`), `M` = its base (origin/main `f1595eb3`), `S9` / `S0` = the #2569 / #2570 heads rebased on `M`
without the transfer PR (`prod/ctl-stack-2569-on-main`, `prod/ctl-stack-2570-on-main`; rebasing apply/2569 08963d79 and apply/2570 317e0cfc from
c4fa3638 onto main has 5 conflicts of its own, all main drift, listed in "Checkpoint log 2"). Result: `prod/stack-2569-on-xfer`, `prod/stack-2570-on-xfer`.

```bash
git rebase --onto prod/xfer-main origin/main prod/ctl-stack-2570-on-main     # 25 picks, 9 stop with conflicts (list below)
# at each stop: take every conflicted file from the commit with the SAME SUBJECT on prod/stack-2570-on-xfer, then continue
git checkout <same-subject commit of prod/stack-2570-on-xfer> -- $(git diff --name-only --diff-filter=U); git rebase --continue
# afterwards cherry-pick the two adaptation commits (or fold them as their messages say):
#   "refactor(dynamic-filter): carry the Bloom publication route through construction"   (F1, after the last #2569 commit)
#   "test(dynamic-filter): keep the union transfer test and counters on unified construction" (F2, after the last #2570 commit)
git diff prod/stack-2570-on-xfer            # must be empty; same check for the #2569 head against prod/stack-2569-on-xfer
```
The replay of exactly this (script + log: `$R/notes/xfer_rebase/replay_rebase.sh`, `replay.log`) reproduces the tested trees.

Textual conflicts (stack commit subject -> files -> resolution):

| # | Stack commit | Conflicted files | Resolution |
|---|---|---|---|
| 1 | extract build-input certification | `cmake/sirius-test-sources.cmake`, `src/op/CMakeLists.txt` | both sides add list entries: keep both (union) |
| 2 | accumulate Bloom filters for every key type | `src/cuda/sirius_dynamic_bloom_filter.cu` | keep the transfer PR's `finish_impl` (validate, request, `unite`); the stack's per-key array sizes feed one `union_shape` per key; the stack's copy of the old chunk pipeline / HOST loop is dropped |
| 3 | stage exact keys and per-batch bounds | `src/cuda/sirius_dynamic_bloom_filter.cu` | includes: both; `create()`: the stack's `lease_partials` first, then the transfer PR's `peer_islands` |
| 4 | unify doc comments and UPPER_CASE enumerators | `src/cuda/accumulation_partial.cuh` | stack text; the helpers stay in `accumulation_cuda.cuh` (see S2) |
| 5 | construct at every build PARTITION | `src/op/dynamic_filter/dynamic_filter_publisher.cpp` | the stack's publisher body with the transfer PR's fields (`publication_route`, `bloom_degraded`) instead of `published_filters::transport` |
| 6 | delete the one-shot deposit path | `detail/accumulated_bloom_builder.hpp`, `test_dynamic_filter_accumulation.cpp`, `test_dynamic_filter_accumulation_fault.cpp` | transfer PR's interface and tests, with the stack's deletions and its tag rename `[multi_partition]` -> `[construction]` |
| 7 | document unified construction | `docs/super-sirius/dynamic-filters.md` | stack text + the transfer PR's transport / workspace paragraphs |
| 8 | unify doc comments in unified construction | `detail/accumulated_bloom_builder.hpp` | stack doc style on the transfer PR's `try_create` / `finish` text |
| 9 | document shadowed keys | `docs/super-sirius/dynamic-filters.md` | as 7 |

Changes git does not flag (no conflict, but needed; they are in the same-subject commits or in F1 / F2):

| # | Where | Change | Why |
|---|---|---|---|
| S1 | F1: `build_filter_construction.{hpp,cpp}` | report `publication_route` + `bloom_degraded` instead of the deleted `publication_transport` | the stack adds a second reader of the publication result |
| S2 | "stage exact keys": `src/cuda/accumulation_partial.cuh` | include `accumulation_cuda.cuh` instead of defining `check_cuda` / `cuda_event` / `make_event_on` | the transfer PR moved them to a shared header (else: redefinition) |
| S3 | F1 + F2: `test_dynamic_filter_union_transfer.cpp` | builder made with per-key block counts (`builder_type::key{..., blocks[i]}`), shapes of different size per key restored, tags `[construction]` | on main all keys share one geometry; #2569 sizes each key; #2570 renames the tag |
| S4 | F1 + F2: `dynamic_filter_stats.hpp` | counter prose keeps `accumulation_publications_degraded` and the new meaning of `..._host_staged`; F2 adds the counter to the construction list | the stack rewrites the same paragraph |
| S5 | F1: `sirius_dynamic_bloom_filter.cu` (one orphan comment), `accumulated_bloom_builder.hpp` (`publication_scratch_bytes` doc: bound per key group) | text only | |

History caveat: the same-subject commits were grafted by tree, so between "extract build-input certification" and F1 (stats prose) and between
"delete the one-shot deposit path" and F2 (union test) the tree carries the control stack's version of those two files; only the two heads are
built and tested. Fold F1 / F2 as their messages say when the real PR branches are rebased.

Check "stack-on-transfer minus control stack = transfer PR + the rows above": `git diff --stat prod/ctl-stack-2570-on-main prod/stack-2570-on-xfer`
= the 28 files of `git diff --stat origin/main prod/xfer-main` (same new files, byte-identical except the union test, S3) + `accumulation_partial.cuh` (S2)
+ `build_filter_construction.{hpp,cpp}` (S1); 3370 insertions / 728 deletions against the transfer PR's 3379 / 723.
