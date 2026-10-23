# S4 implementation kickoff: stop at four-GPU handoff

2026-10-22. This is the implementation contract for the bounded first delivery, supplementary to `/home/ubuntu/sirius/S4_ARCHITECTURE.md`. It does not select a winning topology. No source changes, builds, tests, staging, or commits were performed by this architect.

## Baseline and stopping point

The parent owns `/tmp/sirius-s4-publication`, branch `stacked/df/04-publication`, created from S3c `9c329e31f65be938bf9de4fc823bd29abf0359cc` (PR #2280). The migration prerequisite is **PR #2302**, not #2303: the parent verified/fetched head `cb6e7f9d0a055679b700e30f2da51b1ba331a027` as `s4-prerequisite/pr-2302`; its main parent is `12d312ee02263f0187bf4c91ffd689e8e5d48229`. cuCascade remains `1a9281093a0fe49634594d541d7ce327129f53d6`. Integration completed without conflicts and the parent froze prerequisite tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`. `git merge --quit` preserved that index without creating a merge commit. See [S4_BASELINE.md](S4_BASELINE.md); tracked S4 changes are compared with this tree, with untracked S4 files included separately.

The parent supplied the comparison baseline and released implementation after its baseline validation. Parent retains ownership of shared build registration/integration and all local GPU scheduling, including the already-authorized focused two-GPU checks. No additional user permission is needed for those checks. Existing user/main/worktree files remain intact. This architect's closing scope check does not independently certify the parent's runtime results.

Deliver a runnable, reviewed bundle containing:

- Required bounded HOST fallback and the existing double-buffered direct control.
- One **experimental destination-issued concurrent-ingress pilot**, justified as a bounded comparison because scratch already contains source-disjoint slots. It is not a selected optimization. Preserve root, OR, egress, chunk geometry, and payload bytes.
- Real-pool Stage A pair/all-pairs read/write and root-ingress/egress diagnostics; baseline/pilot Stage B builder and normal-SQL measurements.
- Exact bitmap correctness, negative cases, immutable paired builds, machine/binary manifests, working four-GPU commands, and raw-data analysis.

Then **stop for the user's four-GPU runs**. No tree, ring, distributed OR, alternate physical reducer, source-push publication implementation, or automatic topology selection belongs in this delivery. Stage A can measure source-issued writes without implementing a production push schedule. Existing two-GPU evidence cannot select a four-GPU winner.

## Ownership and interfaces to freeze before parallel development

| Owner | Files/responsibility | Output consumed by others |
| --- | --- | --- |
| Production developer | `src/cuda/sirius_dynamic_bloom_filter.cu`; builder detail header; `dynamic_filter_publisher.cpp`; minimal additions to existing dynamic-filter counters | Fallback/control/pilot implementation; private experiment entry points; measurement field definitions; targeted lifetime tests |
| C++ fixture developer | New experiment executable under `test/cpp/operator/`; existing accumulation utility/tests as needed | Real-pool transport mode, builder mode, and thin normal-SQL mode; versioned JSONL records; complete-byte oracle |
| Python/handoff developer | One `scripts/run_s4_experiments.py`, one `scripts/analyze_s4_experiments.py`, handoff README/config examples | Host/build manifests, SQL preparation/oracle, randomized paired execution, result checks/analysis, ready commands |
| Parent/integrator | Combined baseline; shared CMake registration and private build choice; paired library/extension builds; local test scheduling | Exact resolved paths/hashes and baseline tree; integrated test/reviewer evidence |

Coordinate any shared-header or test-file edits through the production owner; do not let parallel developers independently invent the seam or JSON field names. `cmake/sirius-test-sources.cmake` and `cmake/sirius-tests.cmake` are the existing test-registration locations; do not assume a `test/cpp/CMakeLists.txt` exists.

### Minimal C++ seam

Keep the existing production `accumulated_bloom_builder::finish(root, stream, scratch_mr)` return and lifetime contract. It delegates to the same implementation as the detail-only diagnostic entry point below. `dynamic_filter_publication_session` keeps certification, admission, cancellation, and fanout.

Proposed names are the contract for these developers; implementation may change a name only with both consumers updated together:

| Detail-only interface | Contract |
| --- | --- |
| `compiled_publication_schedule()` | Returns `current_direct` or `concurrent_ingress`, and a stable build-choice string. Choice is compiled into the library, never a mutable global or SQL/runtime setting. |
| `finish_for_experiment(root, stream, scratch_mr, transport, measurement)` | Same optional borrowed filter span as `finish`; `transport` is `automatic`, `force_host`, or `require_direct`. Uses the **compiled** schedule, not an independent fixture algorithm. `require_direct` reports explicit unavailability instead of silently falling back; it is diagnostic only. Caller owns an optional plain measurement record through return/unwind. |
| `inspect_partial(key_index, device)` | Diagnostic host snapshot of an admitted partial after all contribution calls return and before any finish call. Waits on that partial's stream. Returns owned bytes, not a borrowed mutable pointer. Never called on the timed production path. |
| Existing `inspect_replica(filter, device)` | Reuse for complete byte equality and stream-idle checks after finish; no new replica inspection abstraction. |

Use the existing detail header unless a small companion declaration file materially reduces dependencies. Do not expose a graph/scheduler interface. The private transport selector is sufficient for fallback correctness; schedule comparisons use separate library builds.

`publication_measurement` is a fixed-field value, filled without throwing from failure cleanup. Define: compiled schedule; actual transport; root/device count; key count and `B/C`; contributing GPU count; status/refusal reason; input bitmap bytes; ready replica bytes; logical edge bytes and H2D/D2H bytes **actually enqueued**; requested/aligned device scratch and physical HOST-block charge; host prepare-through-completion elapsed. Separate planned bytes from submitted bytes on error. Optional local stage durations carry device ID and an availability flag; no vectors growing after submission and no callback registry. Timing-enabled events are separate from the existing timing-disabled synchronization events.

For SQL, reuse `sirius::test::get_dynamic_filter_stats_snapshot(connection)` from native integration support in a thin experiment-executable SQL mode. Python supplies normal SQL and workload metadata; C++ executes it through the same DuckDB/Sirius integration and snapshots before/after each query. This avoids introducing a public SQL telemetry function. Record GPU-interception evidence; a result/load smoke alone is insufficient. A matched built CLI/extension smoke remains part of the bundle.

Extend the existing counter/snapshot mechanism only for missing measurements: `accumulation_input_bitmap_bytes` counts each contributing GPU's `K*B` once per attempt (not input column bytes); `accumulation_ready_replica_bytes` counts fully completed arrays across devices; `accumulation_published_bitmap_bytes` counts each key's `B` once when actually pushed to at least one channel, not once per binding. Existing success/skip counters remain; distinguish cap refusal from resource refusal where currently aggregated. Report no-publication/unknown bytes explicitly rather than estimate from cap. The production developer owns these definitions and updates field enumeration together. SQL sample records use snapshot deltas only after query work is retired.

Add experimental preparation-through-completion timing to the same record/snapshot path if needed; **do not relabel** existing `accumulation_publication_latency_ns`, which measures final contribution to first push. Per-stage detail may come from fixture/profiler runs; do not add a general telemetry framework to make every query expose every device stage.

## Production invariants and exact resource decisions

Reuse `accumulated_bloom_geometry` and check arithmetic: `P=K*A(B)` per GPU; `C=min(A(clamp(B/8,2MiB,8MiB)),A(B))`; `Q=K*ceil(B/C)`; `h=min(2,Q)`; current root scratch is one allocation of `D=h*s*C`, charge `A(D)`. Here `A` is CUDA allocation alignment and `s` is contributing nonroot GPUs. Persistent arrays remain `N*P`; completed replicas overwrite those arrays. A partial is a full `B`-byte bitmap; it is not a stripe.

Remove the transport-only decline in `try_begin_accumulation`; retain policy/certification/cap/type gates. In finish preparation, the migration's `find_pair_without_pool_access`/`try_grant_pool_access` determines direct availability on actual owning spaces. Unsupported/unverified capability selects fallback. `cucascade::cuda_error` and contract errors propagate; do not turn them into unavailable capability. Keep current `replica_fanout_failures` behavior for ordinary stores; do not transplant its per-destination omission into all-replica accumulated publication.

For fallback, retain the selected replica's borrowed HOST space from `dynamic_filter_replica_space`. Retrieve `fixed_size_host_memory_resource`, call `get_block_size()`, and acquire **one** `allocate_multiple_blocks(C)` allocation before mutation. The returned `fixed_multiple_blocks_allocation` owns the blocks; `.size_bytes()` must equal `ceil(C/F)*F`. Use `.at(i)`/block spans without merging allocation boundaries. The smallest implementation uses the resource's checked allocation directly; do not add a duplicate HOST reservation or claim only `C` physical bytes. If passing an explicit reservation, its owner must outlive the allocation, whose destructor retains a reservation pointer.

For `s>0`, allocate one root `C`-byte device scratch buffer through the task's supplied `scratch_mr` and allocation stream. Root is already tracked: no second scoped reservation. Existing task policy charges only the excess over unused task reservation to global capacity. For `s=0`, root scratch is zero; for no transfers, HOST staging is also zero. This delivery needs no remote device scratch or remote reservation beyond existing partial arrays.

For each chunk: source D2H into staged blocks; checked completion; staged H2D into root scratch; OR; checked completion before reusing either buffer. After all sources, root D2H once, then H2D to each target; retire all readers before staging reuse. Serial synchronization is acceptable for this required fallback. Reuse `device_copy_batch`, pre-reserve descriptors, check `enqueue`'s CUDA status, and remember source access occurs asynchronously. Own both legs' buffers until completion. Do not call the whole-payload ordinary replica helper repeatedly and assume it provides one reusable bounded workspace.

Acquire streams/events, join-list storage, scratch, staging, and descriptors before the first publication copy/OR where practical. A pre-mutation allocation refusal may retry preparation using the smaller fallback only after safely retiring any asynchronous allocation already made. No fallback restart after a publication mutation or uncertain submission. Existing code creates some readiness events late; either pre-create the new schedule's finite event set or include every already-submitted stream in failure draining. Do not make source edits across unrelated modules solely to eliminate every old event allocation.

### Concurrent-ingress pilot: required event graph

Use one exclusive **root-device** ingress stream per contributing nonroot source, with destination-issued copies, and the existing per-source scratch slots. Do not use the target partial/egress stream as its ingress stream. A new stream must be registered for failure draining before its first submission. Scratch has the same `h*s*C` charge as control.

For global chunk index `j`, half `p=j%2`, and source `i`:

1. Record scratch-allocation-ready on the task allocation stream. Record source-ready on each source partial stream after its insert dependencies; record these **before** enqueueing egress. Each ingress stream waits on both readiness events.
2. When reusing a half, ingress stream `i` waits on the prior `merged[p]`, then copies only source `i`'s chunk into slot `(p*s+i)*C`. Record `copied[i][p]` on that ingress stream.
3. Root OR stream waits on **all** `copied[i][p]` and already follows root inserts. OR reads every source slot, writes the root's original chunk, then records `merged[p]`.
4. Each target egress stream waits on `merged[p]` before overwriting that target's original chunk. This chain also proves every source read of that chunk retired before the overwrite. Waiting only on allocation/source readiness does not establish this guarantee.
5. Reuse scratch after OR; egress reads the root array, not scratch, so no unnecessary egress barrier is needed for scratch reuse. Root-array chunks are disjoint and remain alive through egress. Keep `j` global across keys and do not reinterpret halves.
6. Record/wait events in host submission order so each wait refers to the intended event generation before its next record. Final completion and failure cleanup include all added ingress streams plus task/root/target streams. Never reset or destroy these owners while queued readers can run.

No-source and one-source cases still work without fabricating concurrency. Delay tests must cover slow source `i`, delayed OR, delayed egress, both half reuses, and a key transition. Compare identical byte/copy counts against control.

On error, stop submission, retain the cause, drain registered work, then classify/propagate using existing accumulation contracts. Extend `unjoined_gpu_work` retention to HOST allocation, scratch, and **new stream owners**; retaining only device arrays is insufficient. Release task scratch under its matching tracker, persistent partials through the existing untracked retirement path, and never free a remote allocation through an unrelated task tracker. Publish no incomplete replica; completed immutable channel entries remain safe under existing cancellation semantics.

## Fixture and Python protocol

Add one test-side executable, proposed target `sirius_s4_experiment`, linked to the **same library build** as its SQL mode and matched extension. It emits `schema_version=1` JSONL to an explicit output path; diagnostics go elsewhere. Modes are `transport`, `builder`, and `sql`. Argument parsing is test-side, not new engine configuration. Python owns the campaign, not three unrelated runners.

- Transport request: ordered GPU IDs, logical bytes, issuing direction `destination_read|source_write`, traffic pattern `pair|all_pairs|root_inbound|root_outbound`, root, seed, repetitions. Allocate through real Sirius spaces, establish actual pool grants, use nonoverlapping destination regions for concurrent writers, verify every transferred byte, and join before reuse/free. Preserve source immutability. No raw `cudaMalloc` test masquerading as pool behavior. Source-owned stream plus peer-copy submission is a source-issued-copy experiment; report the API/stream owner without claiming it forces a physical route.
- Builder request: GPU IDs, root/final-contributor order, key count, `B`, production-derived `C`, transport `automatic|force_host|require_direct`, seed, repetitions. Use existing `try_create`/`enqueue_add`, snapshot every partial before finish, canonical CPU OR all bytes, then existing `inspect_replica` on **every** replica. Validate no-false-negative membership too. Snapshots/verification sit outside the publication timer. Do not bypass owning pools or implement a second schedule in the fixture.
- SQL request: SQL/preparation file, dataset/profile, query/sample IDs, matched extension configuration, warmups/repetitions, output path. Execute normal SQL through the current production schedule and report timing, result digest/oracle outcome, and counter deltas. Reuse existing native integration setup and snapshot helpers. Python supplies q4/q21/q7 and all22 SQL, pinning/configuration, CPU oracle, and stable sample IDs. No arbitrary test-only physical query plan substitutes for SQL integration.

Shared sample fields: `schema_version`, `run_id`, `sample_id`, `build_id`, `mode`, `process_cluster`, `pair`, `order`, `seed`, `warmup`, GPU IDs/root, compiled schedule, actual transport/API/stream owner, `N/K/B/C`, status/skip reason, submitted logical/D2H/H2D bytes, actual input/ready/published bitmap bytes, scratch/physical HOST bytes, host elapsed ns, local-event durations with device IDs, query elapsed, oracle outcome. Use nullable fields for inapplicable data. Include unavailable/not-measured explicitly. Keep identities and raw measurements rather than only aggregate statistics.

One build manifest records cumulative baseline tree, source diff/hash, dependency pin, compiler/config, compiled schedule, and absolute paths/hashes of fixture, library, extension, CLI, plus runtime resolved library identity. Parent produces `current_direct` and `concurrent_ingress` builds using a temporary **private build-time selector** on the library target. Python rejects mislabeled/identical schedules or wrong runtime library resolution. This is not the existing production SQL config system. Baseline includes the same S4 fallback/measurement support; only the direct schedule differs. Preserve prerequisite-baseline results separately.

Reuse the architecture document's exact workload set and proposed runner commands, implementing them now rather than delivering placeholders. Screen one-key approximately 1/64/near256 MiB and a two-key case under the combined 256 MiB cap; hold out approximately 8/128 MiB plus aligned tails and the stated two-key cases. Default coverage remains 0.9. Raised 2 GiB/approximately 1.46 GiB and SF1000 reproduction are separate optional profiles; SF100/default is the initial SQL handoff, with explicit unavailable-dataset records. q21 SF1000 cap skips cannot count as publication improvements.

Measurement uses host monotonic time around prepare through completion; local CUDA elapsed-time pairs stay on one device. No cross-device timestamp subtraction. Run Stage A before interpreting Stage B. Three warmups/three screen pairs; confirmation infrastructure supports five fresh process-pair clusters times six paired repetitions, randomized matched order, baseline/baseline controls, and cluster-aware intervals. `Warm=min` is never the estimator. Profile scripts must use the intended pinning/iteration settings. Unavailable route counters are not fabricated.

## Simplifications and readiness review

Resolve these plan ambiguities before coding:

1. PR #2302 supersedes the stale #2303 reference; no unresolved PR-number gate remains.
2. The first handoff is not final S4 topology selection. Implement current direct plus one root-pull concurrency pilot; a separate serial-root algorithm, advanced fanout, or push publication can wait for evidence. Stage A serial pair diagnostics are sufficient to start screening.
3. Fixture options alone cannot select a real SQL candidate. Separate compiled-library choices are required; the thin native SQL mode supplies snapshots without public SQL knobs. Future pruning removes the selector after the user's results, not before their first run.
4. “All allocation before mutation” should protect new workspace/lifetimes, not mandate a repository-wide event refactor. Correctly registered failure draining remains essential even with preflight.
5. Source-readiness waits must be copied onto new ingress streams; a wait on the old publication stream does not order other streams. Per-source copied events are mandatory. Existing source-disjoint scratch means no extra device scratch is justified for this pilot.
6. Do not use the existing publication-latency counter as the requested host completion interval or assume fixed-block staging charge equals the logical chunk. Use the explicit meanings above.
7. SQL counter snapshots are connection-lifetime values: take settled deltas. Policy/cap/resource skips stay separate from publication latency samples. A win only above production cap is S5 evidence, not a reason to retain dormant complex code in S4.

Reviewer acceptance for **handoff ready**, not final performance acceptance: bounded fallback publishes canonical replicas; current direct behavior remains represented; pilot dependencies and failure retention are correct; matched binaries/configurations resolve correctly; Stage A and builder/SQL modes actually run through the intended paths; scripts have working help/commands and output validation; parent-coordinated local focused checks and cumulative integration evidence are reported with exact scope; missing four-GPU outcomes are explicitly pending. Include exact commands, build manifest, raw local smoke logs, schema example, workload manifest, and known skips in the delivered bundle. No automatic decision or continuing implementation past the four-GPU handoff.

## Reading record

Reused the completed record in `S4_ARCHITECTURE.md`: Ousterhout chapters 2, 4, 5, 8, 20; C++ Software Design chapters 1 (Guidelines 1–5) and 3 (11–14); C++20 chapters 5 and 7 as supplied, not full books. Architect role, AGENTS, Codebase Design, module-context, prior Document Code/syntax reference, engine lifecycle/memory/dynamic-filter readings, and the narrow DuckDB/Velox/DataFusion consultations are reused; no new book or external architecture survey was needed.

Additional reading for this kickoff: the new cumulative-baseline instructions in `S4_ARCHITECTURE.md`; current `cb6e7f9d` builder declarations and `chunk_pipeline` readiness/half/egress logic; `dynamic_filter_replica_space`, pool helper declarations, admission rejection, dynamic-filter snapshot fields, existing native SQL snapshot usage; fixed HOST block owner/allocation accessors and allocation rounding; current CMake test-registration includes. These are relevant source sections, not full-file or full-codebase claims. Parent supplied the corrected PR #2302 fetch/identity and worktree status. No edits were made outside this kickoff artifact.

## Bounded architecture closing check: first handoff implementation

Assessment: **no architectural scope or complexity blocker found to completing the first four-GPU handoff**. This is a contract/value check of the first-pass interfaces and documentation, not a second functional or lifetime audit and not final performance acceptance. Parent validation and Rawls's functional review remain authoritative for their results. Parent reports a successful first-pass build and exact two-GPU HOST/direct fixture oracles; runtime fault testing is still being completed. The reported constructor stale-handle defect and its remedy belong to that functional review; this check does not independently re-verify them.

The implemented seam follows the kickoff: unchanged production `finish`, detail-only forced transport/measurement and owned snapshots, a fixed measurement value, and one source-specific compiled schedule choice. No new SQL/runtime topology setting, generic collective engine, pool registry, or advanced topology was introduced in the inspected interface/build delta. The existing test-option registration gate used by SQL diagnostics is separate from the immutable schedule choice. Keep `current_direct` the ordinary default until the user's evidence supports a later decision.

The production changes add real required behavior: bounded HOST publication and a small concurrency comparison around existing root/scratch ownership. Diagnostic counters distinguish admitted input, ready replicas, and actually published keys; planned/submitted copy counts also distinguish an unknown accepted prefix on failure. These are useful observability costs for the requested experiment, rather than a general telemetry subsystem. Unavailable local stage events, physical-route counters, and allocator committed peaks remain explicitly unavailable. Such data cannot support stage attribution or route claims until measured; their absence does not prevent the first runnable screen.

Accepted implementation adjustments:

- The native executable statically links `sirius_core` from the production build. Its builder and normal-SQL modes exercise those production objects; they do not dynamically load the separately hashed shared library. The manifest/guide says so. The matched external extension/CLI smoke checks a different loading path and must not be presented as the native SQL benchmark or as independent GPU execution proof. No relinking redesign is required for this handoff.
- The harness has one campaign runner and one analyzer, plus a small paired-build helper. The helper's distinct purpose is freezing comparable binaries and manifests; it is not a third competing experiment orchestrator. The current fixed modes/profiles do not justify a plugin/configuration framework or another abstraction layer. Keep this scope fixed while delivering the first bundle.
- The temporary CMake cache choice is private experiment build machinery on the publication translation unit, not a product setting. Retain it for the user's paired runs, then prune it and losing code during the separately authorized post-experiment finalization.
- The guide preserves the intended stop: return raw observations, never select or deploy a topology automatically. Default-cap skips, raised-cap-only evidence, five process-pair clusters, and separate CPU/native/extension evidence are described without a speedup claim.

No production removal or redesign is requested by this closing check. Before declaring the bundle ready, the parent still completes the in-progress build/fault/functional checks and records exact tested scope. The user's four-GPU screen, held-out confirmation, workload-specific regressions, and final topology decision remain pending. Report the first handoff as runnable experiment preparation, not completed four-GPU S4 validation.

Additional readings for this check: `S4_DEVELOPER_NOTES.md`, `S4_FIXTURE_NOTES.md`, and `S4_EXPERIMENTS.md`; `S4_BASELINE.md`; builder detail declarations for compiled choice, measurement, finish and snapshots; tracked diffstat against the frozen prerequisite tree plus the untracked S4 artifact list; the small build-registration/schedule-choice changes and dynamic-filter documentation delta. Prior personal book/skill readings are reused; no additional book chapters, broad source audit, builds, tests, or GPU work were undertaken. Only this worktree copy of the kickoff was updated; the harness guide and original `/tmp/S4_IMPLEMENTATION_KICKOFF.md` were left untouched.
