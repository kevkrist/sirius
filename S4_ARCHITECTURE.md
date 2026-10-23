# S4: bounded accumulated Bloom publication and measured topology selection

Status: final architecture approval for the four-GPU validation handoff, 2026-10-23, with no remaining architecture findings. The first screen is complete; see [S4_EXPERIMENT_RESULTS.md](S4_EXPERIMENT_RESULTS.md). Current direct publication plus bounded HOST fallback is retained and the losing concurrent-ingress pilot is removed. Local comparisons and Q1 checks across all three revisions passed; Rawls's final review is green with no findings. [S4_FINALIZATION.md](S4_FINALIZATION.md) records the verified evidence and exact prerequisite/control comparison contracts. Final four-GPU acceptance remains pending the user's runs, with no 10% topology-gain obligation for retaining the original direct algorithm. Parent publication uses a separate Git index/ref and preserves local HEAD/index/review changes. [S4_IMPLEMENTATION_KICKOFF.md](S4_IMPLEMENTATION_KICKOFF.md) records the earlier handoff contract.

The final SQL preflight exposed a reproducible `1e-17` Q1 `avg_disc` DOUBLE difference. The reviewed test-only policy `exact_except_q1_avg_disc_v1` permits the existing relative-or-absolute `1e-12` convention only for column 8 of the recognized Q1 schema with exact unique group keys. All other comparisons stay exact; original strict Q8/Q14/Q17 preflights passed. There is no global floating-point tolerance or producer/library runtime change. Original strict comparison manifests and measured harness bytes are preserved. The parent reports green comparator checks (6 cases/43 assertions), 30 CPU harness tests, source hooks, refreshed artifact validation and Q1 across all three revisions passing. All three production libraries remain byte-identical to their prior artifacts, and raw digests are not rewritten to conceal the accepted Q1 difference. See [the oracle correction record](S4_FINALIZATION.md#quantified-q1-oracle-correction).

## Decision and authoritative baseline

S4 delivers bounded accumulated-filter publication when direct peer access is unavailable and preserves the current double-buffered direct pipeline **plus the required bounded host fallback**. The returned screen rejects concurrent ingress: larger builder publication slowed roughly 48–55% and equivalent Q7/Q21 publication slowed roughly 47%. This supports pruning the added complexity, not a claim of universal topology optimality. No advanced topology implementation is justified. Serial host staging alone does not satisfy the direct-path deliverable.

The coordinator remains the final contributor and reducer. It owns completion, failure, cancellation, and publication. No distributed OR, general collective engine, new pool registry, capability cache, NCCL dependency, or production topology-tuning interface is planned.

| Input | Revision/state used here | Authority |
| --- | --- | --- |
| Original S3c input | `9c329e31f65be938bf9de4fc823bd29abf0359cc` | Existing S3c behavior, including fused probe work |
| Main ancestry at prerequisite integration | `12d312ee02263f0187bf4c91ffd689e8e5d48229` | Includes the prerequisite partition/concat changes; not a claim about today's remote tip |
| Separate Sirius migration / verified PR #2302 | Local head `cb6e7f9d0a055679b700e30f2da51b1ba331a027`, parent `12d312ee...`; live PR head verified at implementation kickoff | Required integration prerequisite; not assumed upstream merged |
| cuCascade | `1a9281093a0fe49634594d541d7ce327129f53d6` | Final dependency contracts and source |
| Integrated implementation reference | `origin/reference/df-optimized-20260917`, `ef17c8c55003dbc889e93c24eedf55f127f22c7a` | Implementation ideas only; neither final dependency authority nor S4 results |
| Exact prerequisite baseline | Merge `78a3e34c66143152ff075729f4c018806c11e67d`, tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652` | Untouched production baseline for final S4 end-to-end comparison |
| Finalization starting point | `d925db5dcb909411598b57c566fd9888471d151c`, branch `stacked/df/04-final` | Returned evidence and implementation; new cleanup remains unstaged/uncommitted |

C1 is resolved upstream: GitHub API verification of [cuCascade PR #203](https://github.com/NVIDIA/cuCascade/pull/203) reports `merged=true`, `merged_at=2026-10-21T18:59:18Z`, and exactly the dependency SHA above. This does not establish that the separate Sirius migration is merged. The original #2303 reference returned 404. At implementation kickoff, the migration was identified as [Sirius PR #2302](https://github.com/sirius-db/sirius/pull/2302), open at `cb6e7f9d0a055679b700e30f2da51b1ba331a027`. Its fetched head matches the local migration branch.

At initial integration, S3c was not an ancestor of the main input, which lacked `src/cuda/dynamic_filter_fused_probe.cuh`. The completed prerequisite assembly preserved both parents. The following records that initial procedure, not new checkout or staging instructions for finalization:

1. Resolve and record the current S3c and PR #2302 heads, including the latter's main baseline and cuCascade pin.
2. Create an isolated S4 worktree/branch from S3c and integrate PR #2302, including its main ancestry. Resolve overlaps in dynamic filters, converters, tests, and build registration while preserving both prerequisites' behavior. Existing worktrees remain intact.
3. Keep prerequisite integration separate from S4 implementation. Under the user's no-commit instruction, stage only the resolved prerequisite baseline and record its tree ID without creating a commit. Keep subsequent S4 edits unstaged so VSCode's Changes view shows the S4 delta; prerequisite integration remains separately reviewable under Staged Changes. Do not stage later S4 edits into that comparison baseline.
4. Build the cumulative engine and run the relevant available correctness checks before changing publication. Supply the four-GPU baseline validation to the user as part of the experiment handoff. Neither parent's separate test results substitute for testing the combined code.
5. Begin S4's bounded fallback and experiment harness only after the cumulative baseline is sound. When both parents land, restack S4 onto refreshed main and remove their changes from the final PR diff. Use current CONTRIBUTING rules for any earlier dependent draft; same-repo stacking is maintainer-only. S4 remains one PR, and any later commits/publication follow the user's authorization.

The prerequisites were integrated without conflicts, initially as a frozen index and later preserved in merge `78a3e34c...` with exact tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`. Finalization uses a separate detached prerequisite worktree; its production source remains unchanged. Compare final against both this pre-S4 baseline and the frozen S4 `current_direct` control in separate campaigns: the screened control already includes S4 fallback preparation and instrumentation.

The earlier migration review/design artifacts were removed from the migration worktree during its commit/rebase. The older root design and rollout documents contain superseded `dev`, dependency ownership, and fixed-root assumptions. This document reconciles them using current tracked migration documentation and source; it does not rewrite those documents or link missing artifacts. Historical migration validation is background evidence, not validation of S4 or a claim that the rebased implementation is byte-identical.

The October 5 amendment to [issue #1823](https://github.com/sirius-db/sirius/issues/1823) controls experiment order: pair versus concurrent all-pairs reads/writes, parallel ingress, then conditional distributed OR. S4 follows S3c and the separate dependency migration in the [dynamic-filter rollout](docs/super-sirius/dynamic-filters-implementation-plan.md). No changes to S3c admission policy, Bloom sizing, production coverage threshold, or SQL semantics are included.

## Existing module and the proposed seam

`accumulated_bloom_builder` already owns per-GPU partial arrays, insertion completion, bounded direct scratch, chunked OR/copy scheduling, and completed filter ownership. Keep transport and topology mechanics inside this deep module. `dynamic_filter_publication_session` continues to own certified inventory, exactly-once contribution, admission, cancellation, and channel fanout. The session should not learn copy graphs, scratch layouts, or device-specific event rules.

Current code creates one full-sized partial array per admitted key on every configured replica GPU. Only GPUs that contribute are reduction sources. The final contributor's task calls `finish(root_device, stream, scratch_mr)`. Nonroot contributing partials are copied into root scratch, ORed into the root partial, and complete chunks overwrite the other GPUs' existing arrays. Final replicas reuse partial allocations; publication does not require a second full set of arrays.

The retained `chunk_pipeline` has **source-disjoint slots in each of two scratch halves**. Ingress is serialized on the publication stream, OR runs on the root partial's stream, and egress uses target streams. The evaluated parallel-ingress streams and their additional events are removed during finalization. The integrated reference demonstrates useful half/cross-key ordering, but its older route assumptions and replica allocation scheme must not be transplanted.

Retained internal operations are preparation and execution within the existing builder, not a new public transport abstraction:

| Interface | Required behavior |
| --- | --- |
| `try_create(keys, geometry, replica_spaces)` | Preserve current optional admission and borrowed space lifetime. Retain access to each replica's HOST staging space as well as its GPU space. |
| `enqueue_add(table, device, stream)` | Preserve deduplication, type checks, and insertion readiness. A partial represents all accepted contributions on that GPU. |
| `finish(root_device, stream, scratch_mr)` | Keep the current caller contract. Internally prepare a direct or staged workspace before mutation, execute it, join, then return only complete owned filters. |
| Private publication preparation | Validate device/stream identity, required pool grants, geometry, arithmetic, and all scratch/staging resources. Return a ready owned workspace or the existing optional admission refusal. Typed CUDA/programming errors propagate. |
| Private transport diagnostic seam | Retain existing `finish_for_experiment` with automatic/forced-HOST/required-direct transport and fixed measurements; it uses the same production mechanism. Remove the compiled algorithm selector/getter and schedule measurement field. Cross-revision comparisons use normal SQL with a test-only common-counter fixture overlay, not new production settings or baseline diagnostic backports. |

Use existing `std::span` for borrowed replica/source lists and the existing constrained `join_on_failure` callable interface. Avoid introducing a concept hierarchy or virtual scheduler merely to enumerate experiments. Two real transport implementations justify internal variation; topology hypotheses do not justify a public framework.

### File map for the one PR

| Existing file/module | S4 responsibility |
| --- | --- |
| `src/op/dynamic_filter/dynamic_filter_publisher.cpp` | Replace transport-only early decline in `try_begin_accumulation`; preserve policy gates and final-contributor publication. No CUDA graph logic here. |
| `src/op/dynamic_filter/detail/accumulated_bloom_builder.hpp` | Document private preparation/lifetime contracts and expose only the minimal fixture seam needed to exercise the real implementation. |
| `src/cuda/sirius_dynamic_bloom_filter.cu` | Bounded staged workspace and schedule; retain current direct pipeline; implement only the measured topology winner if one qualifies. |
| `src/op/dynamic_filter/complete_build_inventory.{hpp,cpp}` | Reuse geometry and checked arithmetic. Change only if a concrete workspace calculation requires a small helper. |
| `src/op/dynamic_filter/dynamic_filter_replica_space.{hpp,cpp}` | Reuse borrowed GPU/HOST spaces and current pool-access delegation. |
| `src/op/dynamic_filter/dynamic_filter_replica_reservation.hpp` | Reuse per-allocation aligned accounting and nonblocking remote reservation scopes. No parallel reservation system. |
| `src/memory/pool_peer_access.{hpp,cpp}` and `src/memory/runtime_stream_pool.hpp` | Consume migration contracts; ordinarily no S4 changes. Validate them in the cumulative base. |
| `src/cuda/device_copy_batch.{hpp,cpp}` | Reuse checked directional copies over fixed HOST blocks; ordinarily no changes. |
| `test/cpp/operator/test_dynamic_filter_accumulation.cpp` and its existing utility header | Canonical bitmap, fallback, workspace, ordering, cancellation, and fault cases through the builder/session interface. |
| Existing dynamic-filter multi-GPU, native-shape, and integration tests | Cumulative S3c/main/migration regression coverage and SQL parity; extend only for S4-specific gaps. |
| Retained experiment fixture, handoff runner, and result analysis | Schema-v2 revision-role comparisons, prerequisite SQL-only overlay, and final-only canonical diagnostics under `S4_FINALIZATION.md`. Preserve archived schema-v1 results and their reproducible analysis. |
| `docs/super-sirius/dynamic-filters.md` and rollout docs | Implementation author updates final behavior, selected schedule, limitations, and evidence after experiments. |

## Terms, bytes, and storage

A **partial Bloom** is incomplete in contributing rows, not smaller in bitmap extent. Every contributor's bitmap for a key has the same `B` bytes and hash/geometry. A **complete union** is the bitwise OR of every certified contributor. A **stripe** is a disjoint range of that bitmap. A quarter stripe is approximately `B/4`; a partial is still `B`.

Reduction combines partials. Dissemination copies an already complete union, either as full chunks or completed stripes. Transport selects direct peer-copy APIs or explicit host legs. Topology selects who sends to whom. A valid pool grant plus byte-correct copy establishes access/correctness; it does not establish the physical bus route or bandwidth.

Let `N` be configured replica GPUs, `K` admitted keys, `s` contributing nonroot GPUs, `a` CUDA allocation alignment, and `A(x)=ceil(x/a)*a`, with `A(0)=0`. Current geometry is uniform across keys:

- `B = blocks * bloom_bytes_per_block`, with checked arithmetic and whole Bloom blocks.
- Persistent charge per GPU `P = K*A(B)`; aggregate charge `N*P`. Existing cap applies to `P`, not one key or all GPUs together.
- `C = min(A(clamp(B/8, 2 MiB, 8 MiB)), A(B))`; a transfer tail uses `min(C, B-offset)` actual bytes.
- `Q = K*ceil(B/C)` and `h = min(2,Q)`. Current direct scratch is one root allocation `D = h*s*C`, charge `A(D)`. Other GPUs need no additional device scratch for this pipeline. Source slot `(half,source)` starts at `(half*s+source)*C`.
- Logical persistent bytes are `N*K*B`; reservation bytes are `N*P`. Peak payload for the current pipeline is `N*P+A(D)`, excluding unrelated query allocations, CUDA streams/events, host metadata, and allocator caching. Report those separately rather than calling this total process/GPU memory.

For four contributors and four replicas, the following counts assume one key of size `B`. Multiply by `K` for uniform keys. An edge's logical payload is counted once, not once at each endpoint; these are **not physical bus accounting** or predicted runtimes.

| Reduction and dissemination candidate | Total logical transfer | Hotspot / tradeoff |
| --- | --- | --- |
| Serial root or double-buffered star | `6B`: reduction `3B`, dissemination `3B` | Root RX `3B`, TX `3B`; other GPUs RX/TX `B` each. Concurrency changes scheduling, not bytes. |
| Tree reduction, then tree broadcast | `6B` | Root load can fall; intermediate nodes and dependent stages can become the bottleneck. |
| Stripe reduce-scatter plus allgather | `6B` | Balanced equal stripes: each GPU RX `1.5B`, TX `1.5B`. More events/steps; no promised 2x speedup. |
| Send each full-sized partial to every other GPU, OR locally | `12B` | Every GPU RX/TX `3B`; avoids a single reducer but doubles aggregate traffic. Screen analytically before implementation. |

General all-contributor totals are `2*(N-1)*K*B` for star/tree/striped reduction plus dissemination, versus `N*(N-1)*K*B` for all-full-partials replication. For the existing star with noncontributors, use `(s+N-1)*K*B`; do not apply balanced all-contributor claims to skewed cases. For unequal stripes/trees, account from actual edge lengths.

Dissemination is also screened independently: for a union already complete on one GPU, star or tree broadcast moves `(N-1)*K*B`; a ring forwarding chain moves the same payload but has more dependent hops. This is distinct from ring allgather, which starts with different completed stripes on different GPUs. Evaluate egress separately before crediting a reduction change with its benefit.

### Bounded fallback storage and traffic

The simplest required fallback serializes chunk transport and reuses the existing GPU OR kernel. It needs one staging window of logical capacity `C_h=C`, one root scratch allocation of charge `A(C_h)` when `s>0`, and no remote GPU scratch. It does not allocate a whole-filter host buffer or materialize all contributors on the host.

Use the coordinator's existing HOST staging space and `fixed_size_host_memory_resource`. If its actual fixed block size is `F`, acquire `ceil(C_h/F)` blocks once: **physical staging charge/reservation `H=ceil(C_h/F)*F`**, even when `C_h<F`. Preserve the resource's block boundaries; adjacent pointers do not prove one allocation. The resource's checked allocation is admission, not an advisory free-memory snapshot. With no transfers (`N=1`, `s=0`), staging and scratch are zero. The maximum additional payload charge is `H+A(C_h)`; it is independent of `B`, `K`, and chunk count. Pool committed/cache high-water marks may exceed the logical checked-out charge and must be reported separately.

For each chunk, stage each contributing source D2H, then H2D into root scratch, then OR into the root's existing partial. After all sources, stage the completed root chunk D2H once and H2D to each other replica, reusing that same staged union. For `N>1`, total D2H bytes are `(s+1)*K*B`; H2D bytes are `(s+N-1)*K*B`. For `N=1`, both are zero. Record these host legs separately from the equivalent `(s+N-1)*K*B` logical inter-GPU payload.

Prefer fixed bounded workspace over a tunable staging cache. If the chosen window cannot be allocated, a genuine preflight resource refusal may decline the optional filter. Lack of peer access alone must select this fallback and complete otherwise admissible filters.

## Admission, ownership, ordering, and failures

### Preparation and reservation

Keep the existing inventory/type/row/coverage/per-GPU cap gates. Production defaults remain the existing **256 MiB cap and 0.9 coverage threshold**. A **2 GiB cap** is an explicitly labeled experimental profile; historical **0.45 coverage** is a separate profile and not the production baseline or a hidden optimization.

Current `try_begin_accumulation` declines when `find_pair_without_pool_access` finds an unavailable pair. Remove that transport-only refusal. At publication preparation, ask the migration helper about the allocation-owning spaces and accessor GPUs required by the selected schedule. The existing all-ordered-pairs helper is an acceptable conservative first implementation; narrowing it requires demonstrated need, not a new capability cache. Explicitly unsupported/unavailable selects staged transport; typed CUDA errors remain errors. Byte-correct pool verification and the private-test contracts remain owned by the migration, not reimplemented in S4.

Allocate all schedule workspace, stream/event owners, join-list capacity, and copy-descriptor capacity before the first publication copy/OR. A direct workspace allocation refusal may choose the smaller staged workspace only at this pre-mutation point, after safely retiring any asynchronous allocations already created. Once publication mutates a partial or enqueues work that could do so, never restart through another transport/topology.

The coordinator's task stream already has a reservation tracker. Allocate root scratch through the supplied `scratch_mr` and that stream; do not attach a second `scoped_replica_reservation` or reserve the whole task estimate again. Existing ignore-limit policy permits the excess against global capacity. If the scratch charge is `d` and unused attached reservation is `u`, incremental global charge is `max(0,d-u)`; successful allocation, not a guessed snapshot, is admission. `get_available_memory(stream)` includes global and reserved availability and is **not** an accessor for `u`. No new reservation-growth API is required.

Any conditional distributed topology or different physical reducer must reserve its remote scratch separately and nonblockingly through the existing scoped helper on an appropriate untracked stream/thread context. Reserve exact per-allocation aligned charges, allocate before mutation, then detach unused reservation capacity while live allocations retain accounting. If any GPU refuses, roll back all prepared resources before selecting fallback or declining. Never wait for remote capacity while retaining a blocking set of other reservations. A different reducer is not a free root-selector option: it adds these lifetimes and needs the stronger experimental value gate below.

### Stream and buffer invariants

1. Every contribution is included once, before mandatory scatter; certified original-ID deduplication survives retry/spill. Final publication remains inside the final contributor's task, after required scatter/output ownership and before its final deposit. Keep main's updated partition/concat schedule and verify it cumulatively with S3c.
2. Allocation/memset readiness and every insertion completion precede reading a partial. A new ingress stream must wait on an event recorded after scratch allocation on the task stream, as well as its source's insertion-ready event. Source-issued writes require grants for the destination allocation's owner and the source accessor; do not infer the direction from a successful reverse grant.
3. Concurrent ingress writes distinct source slots. Each source has a completion event per reused half; root OR waits for every source event. Every ingress stream waits for that half's prior OR completion before overwriting it. One aggregate ingress event on a different stream is insufficient.
4. Keep the global chunk sequence across keys. With invariant `s` and `C`, no scratch reinterpretation is necessary. If a justified future layout change reuses storage differently, drain both halves before reinterpretation. Do not introduce layout-changing machinery for the present uniform geometry.
5. Egress waits for complete OR. Overwriting a contributor's original chunk is safe only after its contribution has been read. Any distributed schedule must similarly protect an original stripe until all required readers retire, and retain completed stripes until downstream sends retire.
6. In staged transport, D2H completion precedes H2D. H2D completion precedes reuse of the staging window; root scratch is not overwritten until OR consumes it. A checked host synchronization between these bounded steps is acceptable for the fallback. Every device's calls use its correct device guard and stream.
7. `device_copy_batch::enqueue` returns CUDA status and uses asynchronous source access. Check every status; neither successful enqueue nor descriptor clearing retires a source buffer. Own HOST blocks, device scratch, sources, streams, and reservations until both copy legs and subsequent readers retire. Reserve descriptor vectors before submission and never coalesce across fixed-block allocation boundaries.
8. Final completion joins all root, ingress, OR, egress, and staging streams. Scratch allocation/free stays correctly ordered on its allocation stream after readers finish. Root task-owned storage is released while its accounting context remains valid; remote persistent arrays/workspace must not be freed through an unrelated task tracker. Preserve the existing untracked retirement path and stream-owner lifetime.

### Publication and error semantics

The builder returns only after every required replica for each returned key is complete. Host wrapper allocation must not strand asynchronously used storage: prepare fallible containers before moving array owners, following the current `make_filters` pattern. The session checks cancellation again before fanout. Published filters are immutable and own their device buffers and stream lifetime.

Publication atomicity means **no visible partial bitmap or half-ready replica**. Existing preallocated channel fanout is not a transactional multi-channel commit; a cancellation racing fanout may leave a subset of channels with complete immutable filters. Preserve those safe semantics rather than add a global publication transaction. Every visible filter must independently contain the entire certified union.

Preflight policy/geometry/capacity refusal may end an optional attempt without a filter. Cancellation may suppress publication. After submission, recoverable optional failures may suppress the attempt only after all submitted work is safely retired; they never trigger fallback restart. CUDA/programming errors must retain their typed/contextual failure and propagate after draining. A drain failure must remain visible; the existing `unjoined_gpu_work` protection retains storage if work may still run. Extend protection to any new HOST blocks, streams, and remote scratch, not just the old arrays. Stop new submissions on failure, drain every registered stream, preserve the causal error, and do not publish.

The current migration's `replica_fanout_failures` helper retains fatal errors while ordinary replica fanout drains pending copies. That is useful precedent, not a drop-in accumulated-publication policy: ordinary fanout may skip an individual destination, whereas this builder must finish all required replicas before returning. Keep the existing accumulation classifier/join behavior unless a confirmed defect demands a smaller correction.

## Mandatory experiment sequence and pruning

The sequence below records the original experiment design. Stages A/B have returned screening evidence; the concurrent pilot is rejected and Stage C is not activated. Current work is the bounded finalization in [S4_FINALIZATION.md](S4_FINALIZATION.md). Original candidate-specific build instructions and ingress-stream invariants apply to the historical pilot, not the retained production implementation. The final validation compares revisions and does not confirm a bad pilot.

Experiments precede a final production topology choice within this one PR. They are development stages, not separate production features. Historical four-L40S/PHB measurements in #1823 motivate investigating a root bottleneck but do not describe the user's next host. Historical q4 was approximately 114 MB/GPU and 26 ms; raised-cap q21 approximately 1.46 GB/GPU and 352–365 ms. Neither these figures nor the issue's projected reduction establishes a speedup here.

The [earlier issue comment](https://github.com/sirius-db/sirius/issues/1823#issuecomment-5898684675) reports a pair probe passing while pool copies staged through the host, and a different publication placement overlapping CONCAT. Re-measure actual allocated-pool transfers and current in-task contention. Do not import that overlap or "no end-to-end loss" conclusion into this schedule.

Implementation proceeds through four bounded phases within the **single S4 PR**:

1. Assemble the cumulative S3c/main/migration base, implement bounded fallback, and prepare the baseline correctness/measurement harness. Preserve existing direct behavior and establish byte/charge accounting.
2. Supply the Stage A/B bundle to the **user for four-GPU execution**; the user returns raw measurements and host/binary manifests. Stop topology selection until that evidence exists.
3. Choose at most one conditional finalist from that evidence, prepare any needed experimental build, then have the user run held-out confirmation and SQL comparisons.
4. Prune losing algorithms and all experimental selectors, rebuild the selected production implementation, and have the user rerun focused correctness/performance checks against the baseline. Complete cumulative regression review and documentation in the same PR.

The first handoff supplied bounded HOST fallback, the unchanged default direct schedule, and a compiled concurrent-ingress pilot. Its local checks are recorded in [S4_VALIDATION.md](S4_VALIDATION.md), and its four-GPU screen in [S4_EXPERIMENT_RESULTS.md](S4_EXPERIMENT_RESULTS.md). Cleanup retains the control and fallback. The harness owner updates [S4_EXPERIMENTS.md](S4_EXPERIMENTS.md) for executable revision-comparison commands under the finalization contract; final four-GPU acceptance is still pending.

### Stage A: correctness and bandwidth screen

Capture the actual host, then run a small representative screen on four GPUs with real owning pool allocations: each ordered pair alone, simultaneous all-pairs traffic, and root-focused concurrent inbound/outbound. Compare destination-issued reads and source-issued writes with the actual grants for each direction. Measure isolated and concurrent TX/RX, byte correctness, and aggregate throughput; report observed copy APIs separately from any physical-route evidence. A CUDA memcpy call alone is not proof of PCIe/NVLink routing.

Use the following small predeclared workload set. Let `t=lcm(a,bloom_bytes_per_block)`; round target sizes to whole `t` units and apply the indicated offset. This keeps bitmap and allocation alignment while producing non-full final chunks at the larger sizes. Fixtures exercise valid aligned geometry; SQL retains its production sizing and records the resulting actual bytes.

| Phase/profile | Fixture workload | SQL workload |
| --- | --- | --- |
| Default screen, 256 MiB combined per-GPU cap | One key at approximately `1 MiB+t`, `64 MiB+t`, and `256 MiB-t`; add one two-key case with `B=64 MiB+t` per key. | TPC-H q4/q21 for publication sensitivity and q7 for contention, preferably SF100. |
| Default held-out confirmation | One key at `8 MiB+t` and `128 MiB+t`; two-key cases at `8 MiB+t` and `128 MiB-t` per key. Assert `K*A(B)<=256 MiB` for every case. | Fresh q4/q21/q7 repetitions; all 22 TPC-H queries for broad parity/no-regression coverage at the available declared scale. |
| Separate raised-cap reproduction | 2 GiB cap, one key around 1.46 GiB, aligned to `t`; label this a historical-scale target, not an assertion of the exact historical bitmap size. | SF1000 q4/q21/q7 if data and resources are available; explicitly record missing data or infeasible cases. |

SF100 is the default SQL handoff; SF1000 is optional historical reproduction. If only another scale is available, save that substitution and its actual filter sizes; never relabel it SF100/SF1000. Record dataset absence rather than fabricate or silently omit results. Fix the initial production chunk geometry. Do not cross-product every root, direction, chunk, size, and query. Pair-level tests diagnose bandwidth; only the production builder schedule can establish integration value.

Use the traffic table to screen full-partial replication explicitly. For four contributors it transfers 12B versus 6B for star/striped algorithms and performs redundant OR work. Prune it without implementation when measured aggregate links, root bandwidth, and the resulting optimistic bound cannot meet the practical gain threshold. Only pilot it if extra independent bandwidth makes the doubled traffic plausibly cheaper; a favorable isolated link number alone is insufficient.

### Stage B: root pipeline and independent full-union fanout

Compare a simple serial root schedule with the current double-buffered star. Then pilot concurrent ingress into the existing source-disjoint slots. Test source-issued writes only when Stage A provides directional evidence; otherwise retain the simpler issuing direction. These are the first topology implementation experiments because they preserve bitmap ownership and scratch size.

Sweep roots by forcing each contributor to finish last in the fixture. This changes the existing coordinator/root naturally and needs no runtime root selector. Test all four roots in the screen, then confirm the selected schedule with representative and adverse roots. A separate physical reducer is a conditional proposal only if unavoidable coordinator placement causes a repeatable material deficit.

For an already complete union, compare current star fanout against one topology-informed tree and, where justified, ring forwarding. Keep reduction input fixed so fanout benefit can be isolated. A tree is worth a pilot only when measured pair/all-pairs capacity suggests offloading the root outweighs added dependencies. Prune ring forwarding when its dependent chain has no bandwidth/locality advantage. Do not implement every labeled tree permutation.

### Stage C: one distributed finalist, only if warranted

Proceed when Stage B still has a measured root bottleneck and Stage A demonstrates usable aggregate capacity. Compare one selected tree reduction+broadcast or stripe reduce-scatter/allgather finalist with the best simple pipeline. Choose from the host evidence; do not build a generic collective engine to make the comparison.

For a four-GPU stripe pilot, partition whole Bloom blocks among four owners, with exact tail lengths summing to `B`. Each owner ORs all contributors' data for its stripe; dissemination begins only after that stripe is complete. This is distributed OR with the final contributor still coordinating completion. A direct stripe-owner schedule can use source-disjoint scratch `h*(N-1)*T_i` on owner `i`, where `T_i` is its maximum stripe length within a chunk and `sum(T_i)=C` for full chunks. Charge `sum_i A(h*(N-1)*T_i)` for the individual allocations; each GPU still owns its original full array. No extra full bitmap is assumed. Preflight all remote allocations and barriers before mutation.

Ring reduce-scatter/allgather is a bounded alternative finalist only if the measured topology favors a neighbor schedule over direct stripe ingress. With balanced stripes, each GPU sends and receives `2*(N-1)/N*K*B`; reduction and allgather each have `N-1` rounds. A bounded implementation may use two stripe receive buffers, charge `2*A(T_i)` per GPU if allocated separately, with explicit receive/OR/send/reuse barriers. Unequal tails require an actual edge schedule and maximum receive size, not blindly `B/N`. Reject a proposal that needs unbudgeted full-size snapshots or overwrites a still-needed original stripe. Before its pilot, document the exact chosen edge schedule and owner lifetimes; no ring implementation is preselected by this specification.

If distributed work, tree scheduling, or a separate reducer does not clear the stronger value gate, delete the experiment and keep the simpler direct pipeline. Regardless of topology, preserve the bounded host fallback.

### Stage D: held-out confirmation and decision

Before running, **freeze/save** the experiment manifest in the result bundle: screened sizes, held-out sizes, query list, profiles, seeds, root schedules, metrics, and thresholds. This requires no Git commit. Screen with three warmups and three paired measurements at each representative point; sweep roots only at the representative middle size initially. Confirm only the finalist, using three warmups per condition followed by **five fresh process-pair clusters with six paired repetitions each** (30 measured pairs per comparison). Each cluster contains a fresh baseline process and a fresh candidate process with matching inputs/seeds; randomize baseline/candidate process order and the matched workload order. This supports separate immutable library builds without switching algorithms inside a SQL process. Retain every sample and run a baseline-versus-baseline control under the same process-pair procedure. Held-out sizes and fresh query repetitions provide confirmation, not a second unrestricted candidate search.

Predeclare these default gates: concurrent-ingress/simple fanout changes need at least 10% median publication-time reduction and a paired 95% confidence interval excluding no improvement on the declared representative publication workload. They must show no material end-to-end regression: the upper bound on slowdown is at most 3% on held-out representative queries. Distributed schedules or a separate physical reducer additionally need at least 5% median end-to-end improvement on the declared publication-sensitive query set, with a paired interval excluding no improvement. Default-cap small workloads must pass the no-regression guard independently of raised-cap gains.

Match observations by process-pair cluster, workload, seed, and repetition index; analyze paired log ratios and retain cluster identity. Estimate intervals from independent process-pair summaries (a predeclared small-sample interval with five clusters); a cluster bootstrap may be a sensitivity check, never an IID bootstrap pretending the 30 pairs are independent processes. Report the interval's estimator, cluster count, median, p90, and all raw values. Apply the broad 22-query no-regression check without averaging away individual material regressions. Use baseline noise to label an inconclusive result rather than relaxing thresholds after seeing results. Permit one predeclared extension of five additional fresh process-pair clusters, then stop and choose the simplest qualifying schedule.

Every SQL sample records actual accumulated and published bytes, admitted keys/contributors, publication count, and any cap/policy skip reason. A skipped filter is not a faster publication sample. Compare publication timings only for equivalent admitted work; keep skipped-query end-to-end results separately for parity and regression assessment. q21 at SF1000 may exceed the default 256 MiB cap: report that expected default-profile skip and run the explicitly raised-cap profile for publication measurement. An advanced topology that wins only above the production cap is unreachable under current policy: preserve its experiment for S5 and remove its dormant implementation from S4. Retaining it requires an explicit justified scope change, not silently raising the cap or changing coverage.

## User-run handoff and evidence contract

For the final handoff, use schema-v2 `baseline`/`final` artifact roles and separate prerequisite-versus-final and frozen-S4-control-versus-final manifests. The prerequisite uses a SQL-only test overlay and its actual production engine; unavailable S4 statistics remain null. Final-only canonical builder/HOST diagnostics retain useful accounting. The historical paired-algorithm protocol below remains evidence context; [S4_FINALIZATION.md](S4_FINALIZATION.md) specifies the current minimal seam, local two-GPU smoke and remote four-GPU acceptance requirements.

The following runner/options are **proposed and not yet implemented**. Use one runner with transport/builder/SQL stages and one small analysis script; do not duplicate orchestration across three runners or introduce a benchmark framework. The implementer supplies help text and bounded defaults. Host GPU IDs, data paths, and binary locations are explicit inputs; no machine model or dataset location is assumed here.

| Proposed artifact | Contract |
| --- | --- |
| `scripts/run_s4_experiments.py` | Capture host profile; run Stage A diagnostics, fixture-driven builder schedules, and native pinned-table SQL pairs; produce raw JSONL plus manifest. Support bounded screen/confirm/regression modes, four explicit GPU ordinals, default/raised-cap profiles, CPU oracle, and evidence of the production GPU path. |
| `scripts/analyze_s4_experiments.py` | Validate paired samples and schema, compute predeclared decisions, bytes/peaks, intervals, and held-out outcomes without using warm minima. |
| Test-only experiment fixture in `test/cpp/operator/` | Invoke the same builder/session implementation from the same library as integration. Control contribution order, immutable inputs, and internal candidate options. Avoid a synthetic lookalike schedule claimed as integration. |

For real SQL, produce separate baseline and candidate library builds from the same cumulative source/configuration, differing only in a **private build-time schedule choice**. Each build has its own fixture, library, extension, and CLI resolution manifest. The choice is compiled into the library call used by normal SQL; no mutable global, runtime flag, or SQL setting changes it. The runner launches isolated processes for the paired builds and verifies resolved paths/hashes against the manifest. Use the same compiled choice in each build's fixture and SQL run; fixture-only overrides are labeled diagnostics and cannot support SQL performance claims. Before merge, remove the build selector and losing code, then rerun the chosen production implementation without experimental selection machinery.

The executable handoff is documented in [S4_EXPERIMENTS.md](S4_EXPERIMENTS.md), including paired artifact construction, Stage A/B screening, held-out confirmation, all-query regression, and analysis. The user supplies four visible GPU ordinals and the actual SF100 dataset. The first run is the screen; choose any conditional finalist only after inspecting its evidence. The native fixture statically links the same production objects used by the shared library. The separately frozen extension/CLI pair is also load-checked against its own library.

For optional historical reproduction, use the same runner with `--profile raised2g --scale 1000 --queries 4,21,7` and the actual SF1000 data path; its fixture set includes the approximately 1.46 GiB case. Save it in a separate result directory. Run the default profile at that scale separately to expose cap skips. A manifest may declare unavailable datasets so SQL cases are recorded as unavailable while fixture work proceeds. After pruning, repeat confirmation/regression with a manifest pairing baseline against the final production build; do not substitute the earlier experimental binary's results.

A host manifest records GPU UUID/ordinal mapping, model, VRAM, driver/runtime/toolkit, PCI bus IDs, `nvidia-smi topo -m`, peer capability matrix, CPU/NUMA/pinned-memory placement, process affinity, clocks/power policy, concurrent load, and available profiler/counter support. Record repository/dependency SHAs, dirty diff, binary hashes, resolved shared-library paths, extension/CLI versions, all config values, dataset identity/scale, pinning, warmup policy, and seed. Demonstrate that fixture and integration resolve the same library build; do not silently load an installed older extension or incompatible Python DuckDB binary.

Measure global publication elapsed time using a monotonic **host timer from preparation entry through the final completion barrier**, including admission/allocation and joins. Also report preparation, insertion readiness wait, ingress, OR, egress, host staging, and retirement stages where observable, plus end-to-end query time and publication's position on the query critical path. CUDA elapsed-time event pairs must be on the **same device**. Cross-device waits are valid ordering tools; subtracting cross-device event timestamps is not valid timing. Overlapping local stage durations must not be summed as global elapsed time. Use profiler traces to distinguish overlap and contention.

Raw sample records include `run_id`, process-cluster/pair/order/seed, stage and candidate, root and device IDs, transport API/direction, profile/cap/coverage, `N/K/B/C`, contributor distribution, actual accumulated/published bytes and publication count, cap/policy skip reason, warmup flag, global host elapsed, local stage durations with device ID, query elapsed, logical edge bytes, H2D/D2H bytes, per-GPU TX/RX, requested/aligned scratch, physical HOST staging charge, per-GPU allocator peak/delta, allocator committed peak when available, completion/cancellation/error status, and oracle/parity results. Missing physical-route counters are marked unavailable, not inferred. Save traces separately and link them by run ID; profiling runs are distinct from uninstrumented decision timings.

Reuse existing benchmark manifests, raw SQL timings, configuration, dirty diff, and CPU oracle where practical. `test/tpch_performance/benchmark_and_validate.sh` reports headline **Warm=min**, which is unsuitable for selection; analyze its raw paired distributions. `test/tpch_performance/profile_tpch_nsys.sh` defaults to `ITERATIONS=2`, meaning one cold plus one hot run, and unpinned parquet views. Configure native pinned tables and the intended repetition/profile explicitly or use the purpose runner. End-to-end confirmation must execute the current final-contributor schedule with normal partition/concat overlap, not a standalone copy test.

## Correctness, negative cases, and merge gates

The canonical oracle takes byte snapshots of every contributor before publication mutation and computes CPU bitwise OR over **all bitmap bits** for every key. Compare every byte of every published replica to that union. Checksums and membership-only probes are insufficient: an all-ones bitmap passes no-false-negative membership while being the wrong union. Also test inserted-key membership without false negatives, probe-mask behavior, and GPU/CPU SQL parity, including duplicate/null semantics.

Required S4 coverage is bounded and focused:

- One/two/four GPUs, reordered or non-dense visible ordinals, all possible final contributors, noncontributing replica GPUs, empty/local-only cases, multiple keys, a short final chunk, and both scratch halves reused across a key transition.
- Forced staged transport and genuine unavailable-peer admission: otherwise admissible filters must publish complete replicas, with positive publication evidence. Check staging charge against `round_up(C,F)` and device charge against per-allocation alignment; ensure no allocation scales with full `B` beyond existing arrays.
- Root scratch pressure with partially unused task reservation, remote workspace refusal on a later GPU, HOST block exhaustion, metadata/event/stream failure before submission, and safe rollback. Do not duplicate reservations or miscredit frees to another task.
- Controlled delays/faults before ingress, after one source copy, between halves/keys, after OR, during egress/staging, and at final join. Check no early staging reuse, no fallback restart, no partial publication, and drain-before-release. CUDA failures must escape as failures after draining; unavailable capability is a distinct outcome.
- Cancellation before preparation, during insertion/publication, and immediately before fanout; deduplicated retries, spill/re-upgrade, query drain, and retained owner lifetimes. Preserve completed wrappers until untracked retirement.
- Sanitizer coverage of direct and staged S4 paths, tails, concurrency, and representative failure/cancellation cases. Report exact cases/assertions, explicit API-error reporting, exit status, and logs; do not turn the earlier migration's bounded clean result into a blanket S4 claim.

Use existing cumulative migration pool-permission and typed-error regressions rather than cloning all dependency-only tests into S4. The current helper and `replica_fanout_failures` changes belong in the assembled baseline. Re-run relevant existing S3c/native-shape/integration coverage against main's partition/concat refactors; parent tests from separate checkouts are not proof of the assembled engine.

Before the one PR is ready, require: both parents present; required fallback publishes byte-exact filters; working direct path demonstrated on suitable hardware; all ownership/error tests pass; experiment manifest/raw data and pruning decision recorded; selected schedule passes held-out/no-regression gates; losing knobs and algorithms removed; final documentation and repository checks pass. If suitable hardware or a profiler counter is unavailable, report the exact missing evidence. Missing route telemetry is not a correctness defect; untested direct behavior is an explicit validation gap, not permission to call the direct deliverable complete.

## Reading record and narrow reference lessons

This design reuses the prior architect reading record and adds the performance chapter below. These are substantive chapters/sections actually read, **not full-book claims**:

| Source | Reading record and application |
| --- | --- |
| `/home/ubuntu/AgentDocs/A Philosophy of Software Design.md` | Chapters 2, *The Nature of Complexity*; 4, *Modules Should Be Deep*; 5, *Information Hiding (and Leakage)*; 8, *Pull Complexity Downwards*, reused. Chapter 20, *Designing for Performance* (20.1–20.5), additionally read for this task: measure the critical path and justify complexity with evidence. |
| `/home/ubuntu/AgentDocs/C++ Software Design.md` | Chapter 1, *The Art of Software Design* (Guidelines 1–5), and Chapter 3, *The Purpose of Design Patterns* (Guidelines 11–14), reused. Keep schedule variation private and avoid speculative abstractions. |
| `/home/ubuntu/AgentDocs/C++20 -- The Complete Guide.md` | Chapters 5, *Concepts and Requirements*, and 7, *Ranges and Views*, as supplied, including their draft/incomplete sections, reused. Prefer concrete borrowed ranges and existing callable constraints over a new inheritance hierarchy. |
| Role and skills | Architect role, root AGENTS, Codebase Design, and module-context workflow read. Document Code and its full cuDF documentation syntax reference were read for the prior migration; this task edits a design specification, not C++ API comments. |
| Engine documentation | Prior full Super Sirius README, architecture overview, memory management, multi-GPU, query lifecycle, and data-management readings; relevant pipeline lifecycle sections. Current dynamic-filter publication/accumulation, unified-design H02/rollout, and implementation-plan S4/C1 sections rechecked. Relevant S3C architecture and validation lifecycle, measurement, rollout, and final-review sections read; no whole-file claim for the long validation log. |
| Module context and source | Relevant cuCascade memory-space/reservation, GPU adaptor, fixed HOST-block and stream-pool sections; RMM resource/stream interfaces. Final `1a928109` source controls where generated references differ. Current `cb6e7f9d` builder/session, pool helpers, reservation scopes, transfer/error handling and runtime stream ownership inspected; `ef17c8c` accumulated publication inspected as a historical implementation reference. |
| Migration history | Prior final design/review/validation readings retained as history; removed artifact paths are deliberately not dependencies of this document. Current tracked migration docs and source control present-tense contracts. Current CONTRIBUTING branching guidance read. |

Primary implementation consultations are deliberately narrow; CPU engine designs provide ownership/completeness lessons, not GPU topology or performance evidence:

- [DuckDB `physical_hash_join.cpp`](https://github.com/duckdb/duckdb/blob/main/src/execution/operator/join/physical_hash_join.cpp): Bloom initialization treats published filter ownership/immutability as a lifetime constraint. Apply that to completed replica ownership; do not mutate a visible filter for the next chunk/query.
- [Velox `HashJoinBridge.cpp`](https://github.com/facebookincubator/velox/blob/main/velox/exec/HashJoinBridge.cpp): complete build-result handoff is coordinated under a lock, with waiter notification outside it; pending asynchronous work must be retired on failure. Preserve session ownership without holding its lock across GPU waits.
- [DataFusion hash-join execution](https://github.com/apache/datafusion/blob/main/datafusion/physical-plan/src/joins/hash_join/exec.rs) and [shared bounds](https://github.com/apache/datafusion/blob/main/datafusion/physical-plan/src/joins/hash_join/shared_bounds.rs): shared build accumulation waits for partition completion, and cancellation/unknown contributions cannot justify a restrictive filter. Preserve certified completeness and optional omission rather than expose a partial union.

These upstream source pages were consulted on 2026-10-22 at their then-current `main`; they are conceptual references, not claims about Sirius's pinned DuckDB version. The returned screen supports retaining the original direct schedule with bounded HOST fallback and pruning concurrency. The remaining work is final correctness and end-to-end non-regression validation, not advanced-topology selection. On 2026-10-23 the architect refreshed the exact prior chapter record, result/stopping criteria and relevant source interfaces, and reread Document Code with its complete syntax reference; see the additional reading record in [S4_FINALIZATION.md](S4_FINALIZATION.md).
