# Final QA review: multi-partition dynamic-filter remediation

Reviewed baseline `bdeaa56b9e63bd5dd67924edfe04454fbabc90bd` plus the complete 22-file uncommitted change set (diff SHA-256 `b39c2e77bb2def3fecdaec38936f78d4ac36934401d5f572256510359ab0883f`), including ignored `DYNAMIC_FILTER_REPORT.md`.

## Findings

### [Medium, non-blocking] No deterministic finalization-versus-contribution regression

**Defect.** Tests overlap an in-flight duplicate and two final contributions (`test/cpp/operator/test_dynamic_filter_publisher.cpp:983-1069`) and test incomplete hash-join finalization sequentially (`test/cpp/operator/test_dynamic_filter_publication_claim.cpp:253-268`), but never overlap `on_finalize_operator()` with an in-flight final contribution. The accumulator mutex and hash-join CAS protocol are sound by inspection (`src/op/sirius_physical_hash_join.cpp:898-920,2218-2250`), but this correctness-critical cross-layer linearization remains uncovered.

**Required property.** Deterministically cover both winners: finalize first yields no fan-out and one failed-stat commit; contribution first yields one fan-out and one finished-stat commit.

**Cheapest remedy.** Add one narrow synchronized hash-join test around the existing post-insert seam (or an equivalent join-level seam), reversing release order for the two cases. No physical multi-GPU hardware is needed.

**Release impact.** Non-blocking for this default-off implementation; add it before enabling the feature by default.

### [Low, non-blocking] Concurrency-test rendezvous can hang instead of fail

**Defect.** The duplicate test waits indefinitely on latches (`test/cpp/operator/test_dynamic_filter_publisher.cpp:991-1013`); the competing-final-contributions test uses an uncancellable barrier followed by blocking `future::get()` calls (`test/cpp/operator/test_dynamic_filter_publisher.cpp:1033-1053`). A worker exception before rendezvous, or a synchronization regression, can hang until the outer CI timeout.

**Required property.** Test rendezvous must fail within a bounded interval and release/join every worker on every exit path.

**Cheapest remedy.** Use a bounded condition-variable/promise rendezvous with scope-guarded release, or an equivalent harness timeout that guarantees worker release.

**Release impact.** Non-blocking; this affects failure diagnostics and CI latency, not production behavior or the validity of a passing run.

## Prior findings: resolved

- **Captured task-stream lifetime:** resolved. CUCO partials are constructed on the owning GPU memory-space stream and initialized before task-stream insertion (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:595-615`). The short-lived-task-stream teardown regression is at `test/cpp/operator/test_dynamic_filter_publisher.cpp:953-980`.
- **Aborted statistics dropped:** resolved. The winning `ACCUMULATING -> FAILED` caller folds the aborted outcome exactly once (`src/op/sirius_physical_hash_join.cpp:910-920`), covered at `test/cpp/operator/test_dynamic_filter_publication_claim.cpp:270-305`.
- **Missing overlap/replica-failure tests:** resolved for the prior review's required cases. Tests now force an in-flight duplicate, competing final contributions, and strict-replica failure before fan-out (`test/cpp/operator/test_dynamic_filter_publisher.cpp:983-1100`). Hooks default empty, and only tests supply non-empty hooks (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:653-667`).

## Additional acceptance checks

- Root reduction uses one durable root stream, synchronizes on success, and best-effort drains before failure escapes (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:468-502`). Scratch and non-root partials are released only after successful drain.
- Accumulator abort/publication is mutex-serialized; a single hash-join CAS winner commits terminal state and statistics. Strict replication completes for all active keys before any fan-out (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:513-535,636-649,679-699`).
- `enable_dynamic_filter_multi_partition` defaults false and is passed by the planner only under the `enable_dynamic_filter` master gate (`src/include/sirius_config.hpp:137-142`; `src/sirius_extension.cpp:2351-2365`; `src/planner/sirius_plan_comparison_join.cpp:404-418,661-676`).
- Updated docs and `DYNAMIC_FILTER_REPORT.md` consistently describe the default-off rollout, whole-build invariant, durable-stream ownership, strict replication, and deferred physical multi-GPU validation.
- No new production leak or data race was found. The code uses scoped RAII ownership/locking, atomics, `std::span`, and C++20 synchronization facilities consistently with the reviewed project references.
- `git diff --check HEAD` passed. Developer-supplied evidence reports a release build passing 15/15 targets and focused tests passing 284 assertions in 18 cases. Physical multi-GPU tests were not run, as requested.

## Verdict

**Accepted.** All three prior remediation gates are resolved and no production correctness blocker remains. The two residual test findings are explicitly non-blocking while the feature remains default-off. Physical multi-GPU validation remains required before claiming production validation or changing the default.

---

# Follow-up QA review: per-GPU Bloom policy cap

Reviewed baseline `4016c598332210ab4e4a38b44f7ccb129c5abd1c` plus the final 20-file uncommitted cap change set (diff SHA-256 `976d682c5b268a44a11fc0bfec3f85f3a9582a890dd09a34fdfca5ec7b06a9a3`). This section is scoped to the Bloom-cap follow-up; the prior review and its two non-blocking residual findings remain unchanged above.

## Findings

No open cap-specific finding remains.

## Findings resolved during this review

### [Medium] Saturated estimates could pass an unlimited cap

**Defect.** `estimated_bytes()` used `SIZE_MAX` as the saturation sentinel when raw multiplication or CUDA allocation alignment was not representable, but the initial helper admitted `bloom_budget_allows(SIZE_MAX, 1, UINT64_MAX)` by equality. The constructor also admitted the alignment-only boundary with raw bytes `SIZE_MAX - 31`; CuCascade's subsequent 256-byte `rmm::align_up` can wrap that accounting charge to zero.

**Required property.** A saturated or otherwise nonrepresentable tracked footprint must be rejected before CUCO allocation, while equality remains admissible for representable footprints.

**Cheapest remedy.** Reject the existing sentinel in the budget helper and mirror the alignment-overflow guard in the public empty-Bloom constructor; no new sizing abstraction is required.

**Resolution.** The estimator now uses overflow-safe geometry and saturation (`src/cuda/sirius_dynamic_bloom_filter.cu:53-64,248-255`), the helper rejects the sentinel (`src/include/op/dynamic_filter/dynamic_filter_source_policy.hpp:79-98`), and the constructor rejects raw and alignment overflow before constructing CUCO storage (`src/cuda/sirius_dynamic_bloom_filter.cu:266-285`). Tests pin both boundaries (`test/cpp/operator/test_dynamic_filter_source_policy.cpp:194-206`; `test/cpp/operator/test_dynamic_filter_publisher.cpp:378-391`).

### [Low] Configuration-to-statistics behavior lacked one crossing test

**Defect.** Initial tests covered SQL setting storage and direct publisher outcomes separately. Omitting planner transport, hash-join folding, or snapshot exposure would have left them green.

**Required property.** A configured rejection must preserve results, complete fail-open, construct and push no Bloom, and increment the cumulative skip counter through the production planning path.

**Cheapest remedy.** Add one zero-cap section to the existing deterministic forced multi-partition integration case.

**Resolution.** The section asserts result parity, an enabled producer, a positive size-gate count, zero membership construction, successful publication, and zero fan-out (`test/cpp/integration/test_gpu_execution_dynamic_filter_sip.cpp:100-131,365-375`). It crosses planner transport (`src/planner/sirius_plan_comparison_join.cpp:654-677`), outcome folding (`src/op/sirius_physical_hash_join.cpp:89-102`), and snapshot exposure (`src/include/op/dynamic_filter/dynamic_filter_stats.hpp:118-132`).

## Acceptance checks

- Units match CUCO's 32-byte Bloom blocks and CuCascade's 256-byte tracking alignment; aggregate admission uses division and rejects the saturation sentinel.
- One-shot rejection gates the complete Bloom candidate set before construction while exact IN-lists and zone maps remain eligible (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:223-295`).
- Multi-partition rejection uses the global row count and all active keys before partial allocation, emits no Bloom, and completes fail-open after exact batch accounting (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:410-449`).
- The cap is per join on each GPU: it is neither multiplied by replicas nor divided by partitions. Source/partial storage remains allocator-accounted; destinations reserve their aligned allocation (`src/cuda/sirius_dynamic_bloom_filter.cu:350-385`). Scratch and host overhead are documented exclusions.
- The 256 MiB default is consistent across `operator_params`, YAML, SQL UBIGINT registration, planner capture, outcome folding, and the atomic snapshot.
- No new raw ownership, leak, or data race was found. RAII ownership/reservations, immutable policy, mutex-serialized accumulation, and atomic statistics remain consistent with the reviewed C++ guidance.
- User, design, report, and Doxygen text consistently cover representable equality, scope, all-or-none rejection, zero-cap behavior, reservation distinctions, excluded scratch, statistics, and deferred physical multi-GPU validation.

## Validation

- Full post-cap release build: 222/222 targets passed; post-review remediation rebuilt 52/52 incremental targets.
- Final `[bloom_budget]`: 50 assertions in 6 cases passed, including both overflow regressions.
- Final forced multi-partition parent case, section `a multi-partition build obeys the subordinate switch`: 166 assertions in 1 case passed, including zero-cap config-to-stats fail-open behavior and result parity.
- Broader publication/accumulator/reduction selection: 143 assertions in 14 cases passed. YAML/multi-partition SQL selection: 19 assertions in 2 cases passed.
- `git diff --check HEAD` and `clang-format --dry-run --Werror` over all changed C++/CUDA files passed. The temporary test-only GPU-pool reduction was restored exactly; final status contains only the intended 20 modified files and no backup artifacts.
- Physical multi-GPU execution remains deferred as requested; static placement/reservation review is not a substitute for that hardware matrix.

## Verdict

**Accepted.** The cap is correct for one-shot and multi-partition publication, overflow-safe at policy and constructor boundaries, consistently wired and observable, fail-open on rejection, and covered by focused and end-to-end single-GPU tests. No cap-specific correctness, ownership, race, documentation, style, or test blocker remains. Physical multi-GPU validation is still required before claiming multi-GPU production validation or enabling the multi-partition feature by default.

---

# Prototype QA review: partition-specific multi-build-batch dynamic filter

Reviewed baseline `13e0eb4078451f4b9a666e0bcf4a1d90dfb805f8` plus the settled implementation, documentation, and test change set (diff SHA-256 `db4638981a1dbecdf4430fd07628ca82c322733e588975a145d60ea2ad2b93bf`). This section is scoped to the partition-specific prototype and preserves the two preceding reviews unchanged. The unrelated dirty `.claude/claude-tools` submodule is excluded. Physical multi-GPU execution remains deliberately deferred.

## Findings

### [Medium, non-blocking] Build-CONCAT completion versus bank sealing lacks a deterministic cross-layer regression

**Defect.** Bank tests exercise contribution and sealing, and pipeline tests exercise scheduling, but no test holds a real build-CONCAT task inside contribution while the pipeline attempts finalization. Static inspection finds the production ordering sound: task creation is counted before the pipeline status lock is released, contribution completes synchronously in the task's operator path, task destruction records completion afterward, and pipeline finalization requires created and completed counts to match (`src/pipeline/sirius_pipeline.cpp:406-484`; `src/pipeline/gpu_pipeline_task.cpp:328-339`; `src/op/sirius_physical_concat.cpp:301-308`). A retry also creates its replacement while the original remains counted. Nevertheless, a future refactor could violate this whole-build publication invariant without failing the current tests.

**Required property.** The bank must remain unsealed while any build-CONCAT task is alive or executing its sink path, and it must seal exactly after the last such task completes.

**Cheapest remedy.** Add one narrow pipeline-level test with a synchronized build-CONCAT sink seam: hold a contribution, show that finalization cannot seal, release it, destroy the task, then show that finalization seals and the resulting filter contains the held rows. No physical multi-GPU hardware is required.

**Release impact.** No production race was found, so this is non-blocking for a default-off prototype. Add the regression before enabling the feature by default.

### [Low, non-blocking] The bank concurrency test can hang instead of reporting a bounded failure

**Defect.** The same-partition serialization test uses an uncancellable `std::barrier` and then blocking `future::get()` calls (`test/cpp/operator/test_partition_dynamic_filter_bank.cpp:346-395`). If async task creation fails, or a worker throws before reaching the barrier, the remaining workers can wait until the outer CI timeout.

**Required property.** Every failed test setup or worker exit must release and join all workers within a bounded interval.

**Cheapest remedy.** Replace the barrier with a bounded, cancellable rendezvous whose owner releases waiters during stack unwinding, or use an equivalent harness primitive that provides bounded failure and guaranteed joins.

**Release impact.** Non-blocking; this affects test diagnostics and CI latency, not production behavior or the validity of a passing run.

### [Low, non-blocking] Admitted RIGHT and SEMI joins lack execution-parity coverage

**Defect.** Constructor-level tests verify that INNER, RIGHT, and SEMI are admitted and unsafe join types are rejected (`test/cpp/operator/test_physical_concat.cpp:666-692`), but the end-to-end filtering case executes only INNER (`test/cpp/integration/test_gpu_execution_dynamic_filter_sip.cpp:443-493`). Static semantics are correct: filtering a probe row absent from the complete build cannot change INNER, RIGHT, or SEMI output, and the RIGHT path retains the complete build side for unmatched-build emission. The two additional admitted paths are nevertheless not protected against integration regressions.

**Required property.** With the local filter on and off, RIGHT and SEMI must match CPU results, remain fail-open, and preserve unmatched-build output when the probe is wholly filtered.

**Cheapest remedy.** Add compact parameterized execution cases for RIGHT and SEMI, including an all-pruned probe case for RIGHT, reusing the existing result- and statistics-delta helpers.

**Release impact.** Non-blocking for this prototype; add before a default-on or production-readiness claim.

### [Low, non-blocking] Active-executor GPU-count precedence is not pinned by a regression test

**Defect.** The converter correctly treats a nonempty active-GPU ID list as authoritative and falls back to the explicit heuristic count only when that list is empty (`src/pipeline/sirius_pipeline_converter.cpp:484-510`). No focused test supplies deliberately different values, such as `num_gpus = 8` with active IDs `{2, 5}`, to prove that sizing, cap multiplication, and routing all retain `G = 2`.

**Required property.** Every downstream partition consumer and partition-specific cap calculation must use the same active executor set that task routing uses, including sparse device IDs.

**Cheapest remedy.** Add a converter/build-context regression with mismatched raw and active counts and assert the propagated count and sparse ID mapping. Hardware execution is unnecessary for this structural test.

**Release impact.** Non-blocking while multi-GPU validation is deferred and the feature is default-off; include it in the multi-GPU readiness gate.

## Findings resolved during this review

- **Probe-CONCAT cold-start reservation:** local filtering now saturatingly reserves `3 x stats.bytes` for one input and `4 x stats.bytes` for multiple inputs, covering old/new filtered tables, a BOOL8 mask bound, and merge storage. Zero-, one-, multi-input, build-side, and overflow cases are pinned (`test/cpp/operator/test_no_history_peak_memory_estimate.cpp:146-162`).
- **CUDA device context and failure drain:** contribution selects its recorded device internally, and exceptional stream draining re-establishes that device before synchronization (`src/op/dynamic_filter/partition_dynamic_filter_bank.cpp:250-338`). Bloom replicas retain their allocation device and release device-local storage under the appropriate device guard; arbitrary caller current-device state does not control destruction.
- **Configured versus active GPU count:** the active executor list now determines `G` whenever present, so routing, per-partition sizing, and the worst-case per-GPU cap use one topology. Empty-list construction remains available to no-engine unit tests.
- **End-to-end shape and multi-batch proof:** the integration case now forces STANDARD/nonbroadcast partitioned execution and tiny scan/CONCAT batches. It proves local filters were constructed, successful build fragments exceed filter/partition count, probe rows are reduced, GPU on/off and CPU results agree, and neither ordinary global Bloom construction nor scan fan-out occurred (`test/cpp/integration/test_gpu_execution_dynamic_filter_sip.cpp:443-493`).
- **Cap boundary:** focused coverage admits the exact worst-case per-GPU footprint and rejects one byte below it for multiple keys, uneven partitions, and multiple GPUs (`test/cpp/operator/test_partition_dynamic_filter_bank.cpp:238-266`).

## Acceptance checks

- Each nonempty build fragment contributes synchronously to exactly its hash partition on its actual GPU. A per-entry mutex serializes fragments for the same partition while the lifecycle shared lock permits different partitions to progress independently. Successful contribution statistics are committed only after insertion and stream synchronization.
- Sealing cannot expose an incomplete filter. Any contribution, device, ordinal/type, allocation, or insertion failure clears partial state and transitions the bank to fail-open behavior; missing filters and probe-device mismatches pass rows through.
- Probe CONCAT waits on its sibling build CONCAT only while the bank is pending or accumulating. Existing recursive readiness propagation and build-first dependencies provide a wakeup path; no task-readiness cycle or lost-wakeup deadlock was found. Delim-join inner hash joins are excluded from local-bank planning.
- Partition routing uses sorted active GPU IDs, and retry preserves the preferred device. Probe lookup requires the same `(partition index, device ID)` pair; there is no implicit cross-device merge, replication, or dereference.
- Multiple build batches accumulate bits into the partition entry rather than retaining input batches. Geometry uses the blocking partition row count; skew can increase false positives but cannot create false negatives.
- The policy cap is overflow-safe and accounts for active keys, estimated rows per partition, and the maximum number of local partitions on any active GPU. Durable Bloom allocations use the allocator-accounted memory resource and fail open on allocation failure.
- Planner and physical checks admit only ordinary equality keys with supported exact storage types and probe-filter-safe INNER, RIGHT, and SEMI joins. Null-equal and cast-shaped conditions are rejected; composite matches cannot be falsely rejected because each admitted component filter is complete for its partition.
- Ownership is explicit: the bank owns partition entries, entries own mutable/shared Bloom handles, selections share immutable sealed filter sets, and sibling/statistics pointers are documented non-owning references with enclosing-plan lifetime. No raw owning pointer, leak, use-after-free, or production data race was found.
- The master and partition-specific switches are consistently subordinate and mutually exclusive, and the new strategy remains off by default. API/Doxygen and prototype documentation describe lifecycle, topology, cap scope, fail-open behavior, statistics, and the deferred multi-GPU boundary consistently.

## Validation

- Final post-documentation release build: 196/196 targets passed in 71.7 seconds. Earlier remediation builds also passed completely.
- Forced STANDARD/nonbroadcast, partitioned, multi-fragment end-to-end case: 85 assertions in 1 case passed, including CPU parity, master-off parity, row reduction, fragment completeness, and zero failures/global pushes.
- Combined focused selection `[partition_dynamic_filter_bank],[partition_dynamic_filter],[config_opt][dynamic_filter],[accumulator],[bloom_reduction],[publication_claim]`: 313 assertions in 28 cases passed.
- `clang-format --dry-run --Werror` passed over every changed or new C++ file. `git diff --check` passed, and no source/test/documentation `.orig` or `.rej` artifact remains.
- Physical multi-GPU execution and performance comparison were not run, as requested. Static topology, device-lifetime, and reservation review is not a substitute for that hardware matrix.

## Verdict

**Accepted as a default-off prototype.** No functional-correctness, ownership, race, deadlock, cap-accounting, eligibility, API, documentation, or style blocker remains. Before enabling the strategy by default or claiming production multi-GPU readiness, run the physical multi-GPU correctness/performance matrix and close the cross-layer seal-ordering regression; the three low-severity test gaps should be included in that readiness work.
