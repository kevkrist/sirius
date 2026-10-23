# S4 developer notes

Scope: `/tmp/sirius-s4-publication`, branch `stacked/df/04-publication`. Comparison baseline is the parent-frozen prerequisite tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; HEAD remains S3c and prerequisite integration remains in the index. S4 edits are unstaged. No staging, commits, own builds or GPU tests performed by this developer.

## Readings before implementation

Reused substantive completed developer readings (not whole-book claims):

- C++ Software Design: Chapter 2, Guideline 6 (behavioral subtyping/LSP examples, criticisms and meaningful abstractions); Chapter 7, Guideline 30 (abstract copying, const clone/unique ownership, prototype-versus-variant and shortcomings).
- C++20 -- The Complete Guide: 10.2 Spans Considered Harmful; 10.3 introduction; 10.3.1 Performance of Spans, including 10.3.1.1 Type Erasure and 10.3.1.2 Spans versus Subranges; 10.3.2 const Correctness of Spans.
- CppCoreGuidelines: I.11, F.16, R.1, E.6 and Con.2.

Reread for S4: developer.toml; worktree AGENTS.md; Super Sirius README; the entire S4_IMPLEMENTATION_KICKOFF.md and S4_ARCHITECTURE.md; Document Code SKILL.md and its full references/cudf-documentation.md; module-context SKILL.md. The kickoff corrects the architecture's stale PR number to actual PR2302 and bounds this delivery to control/fallback/one pilot before four-GPU selection.

New relevant documentation: dynamic-filters.md Publication and consumption (including Publication ownership and completion and Multi-partition build accumulation), Ordering and correctness, Configuration and Limitations; full query-lifecycle.md; memory-management.md Memory Reservations, reservation_aware_resource_adaptor, Caller reservations for HOST conversions, Pinned Host Memory, and Stream-Ordering Discipline. Prior full architecture-overview, memory-management, multi-GPU architecture and data-management readings remain reused.

Dependency context: full generated RMM cuda_streams/resource_refs/device_containers/error_handling references; cuCascade memory module Pool peer access and API sections Tier, memory_space_id, memory_space, reservation, memory_reservation_manager, reservation_aware_resource_adaptor, fixed_size_host_memory_resource. Actual checked-out 1a928109 source overrides stale generated signatures. Read fixed HOST allocation owner/block access/destructor and allocation implementation; runtime CUDA batch source-access-order documentation from the installed header.

Source review: complete current accumulated builder CUDA file/detail header, publication/admission/fanout/counter paths, geometry definition/calculation, replica-space and reservation helpers, actual pool grant helper, runtime_stream_pool ownership, device_copy_batch, accumulation fixtures and affected tests. The parent inspected ef17c8c's historical serial-ingress reference; this implementation follows the current source and kickoff, not that older whole-copy/replica design.

Reading checkpoint sent before source edits. Parent released the source gate after reporting native combined-baseline build success, 89 cases/5572 assertions and isolated migration CUDA faults 4 cases/228 assertions. These are parent-reported prerequisite results, not S4 validation.

## Implementation in progress

The detail header fixes the fixture seam: compiled schedule string, diagnostic transport enum, caller-owned fixed measurement, owned pre-publication partial snapshots. Source-specific SIRIUS_S4_CONCURRENT_INGRESS selects the schedule; the parent owns CMake.

Control preserves source-disjoint double-buffered scratch. The pilot adds exclusive root ingress streams, allocation/source readiness, per-source copied events and half reuse after OR. Egress waits for completed OR before overwriting source partials. Global chunk sequence spans keys. Readiness/final events are prepared before publication copies.

HOST fallback borrows the root replica's HOST resource, allocates one checked fixed-block window, and preserves each physical block boundary. D2H/H2D/OR are explicitly joined before workspace reuse. Root scratch uses the supplied task allocator; no duplicate reservation or remote scratch. Direct scratch refusal may choose the smaller fallback only during preparation.

Cleanup takes the final registered-stream span after preparation, drains before releasing owners, and retains the whole builder plus pipeline/ingress owners, scratch and HOST allocation on unjoined work. Submitted-byte counters count successful submissions; submitted_bytes_complete is false after a failed copy call because an accepted batch prefix may be unknown. No copy algorithm is duplicated in the fixture.

Counters distinguish logical input bitmap bytes, ready replicas and per-key actual channel publication; old final-contribution-to-first-push latency is unchanged. New preparation timing and counts use the existing snapshot mechanism. GPU/local stage timings remain unavailable in the fixed record rather than fabricated.

## Validation ownership

Parent owns compilation, GPU tests, sanitizer runs and their logs. Results below are attributed to the parent. Four-GPU performance/topology, non-dense device visibility and held-out SQL measurements remain user handoff evidence; two GPUs cannot select a winner.

## Implementation and test checkpoint

Production first pass compiled in the parent's current-direct build. The initial include exposed namespace lookup collisions; the necessary CCCL qualifications were made explicitly in dynamic_filter_probe.cuh, dynamic_filter_fused_probe.cuh and sirius_rmm_cuco_allocator.cuh, in addition to the builder CU. No algorithm change in those three headers.

Reviewer R1 found that a throwing pipeline constructor could leave newly created ingress handles in the external drain list after their owners unwound. Registration now occurs only after successful construction, into pre-reserved join-list storage, before submission. The isolated regression injects cudaEventCreateWithFlags failure at ordinal eight, after the candidate's first ingress stream owner exists, and checks unchanged partial bytes, allocator recovery and absence of invalid-handle cleanup.

New normal tag `[s4_publication]`: complete CPU OR of all partial bytes on all replicas, both transports, roots 0/1, zero/root-only/two contributors, one/two keys, three chunks with tail and cross-key half reuse; local-only zero workspace; HOST exhaustion with unchanged partials; partially used task reservation with exact scratch peak and no duplicate reservation; logical per-key publication across two channels. Existing unknown-pool and SQL integration expectations now require positive bounded HOST publication. The prior direct-scratch refusal test now expects smaller HOST fallback success. Existing one-key-ignored fanout checks input/ready/published byte distinctions.

New isolated tag `[s4_publication_fault]`: API errors at source/egress copies, half/key transitions, event recording, staging and final join; exact native CUDA cause, every observed submitted stream drained, storage recovery and zero ready replicas; delayed source copies, OR completion and egress are checked against the full-byte CPU oracle. Wrappers are test-only in the existing isolated executable; no production fault hook or callback was added. Parent owns the five extra linker wrap symbols and test registration.

The parent reported control library build success and one forced-HOST native builder smoke with two GPUs/two keys, tail B=2097408 and root=1, exact byte and no-false-negative checks passing. These are parent observations; normal/fault suite results and candidate validation are pending. No direct or staged sanitizer claim is made yet. The fixture/harness implementation belongs to the other developers, and their work is not attributed here.

Parent-coordinated focused commands for each compiled schedule, from this worktree:

```bash
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_unittest '[s4_publication]'
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_cuda_fault_test '[s4_publication_fault]'
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_unittest '[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]'
```

These are handoff commands, not runs by this developer. The parent freezes/copies the binaries and selects the matching build before each run. Local static checks performed: `git diff --check` and the installed clang-format on changed line ranges (whole-file format only for the new isolated test). Comments outside changed behavior are preserved.

Coverage boundaries: the local machine has two GPUs, so concurrent ingress from multiple nonroot sources, four roots and non-dense visible subsets require the fixture's four-GPU handoff. The new failure tests simulate recoverable host-side API errors followed by successful draining; they do not deliberately poison CUDA or claim to validate every real device-loss/unjoinable-work failure. Whole-owner retention for unjoined work is source-reviewed; the existing generic join classifier tests remain separate evidence. No remote scratch exists in this bounded design, so a later remote workspace refusal is not an applicable new path; existing later-GPU partial-lease rollback remains covered. Timing-stage and physical-route measurements absent from the fixed record remain explicitly unavailable.


The first parent fault run reported 7 cases, 5 passed/2 failed, 750 assertions, 745 passed/5 failed (`/tmp/sirius-s4-current-fault-tests.log`). These were test-injection failures, not passing staged fault evidence: three expected copy errors did not escape and two delay injections were unconsumed. Object-symbol inspection proved `device_copy_batch.cpp.o` calls out-of-line `cudf::detail::memcpy_batch_async`; executable `--wrap=cudaMemcpyBatchAsync` cannot intercept the shared library's internal call. Diagnosing Bugs was read and applied using the parent's failing test as the feedback loop; no independent GPU job was started. The isolated wrapper now targets the existing Sirius `device_copy_batch::enqueue` boundary and forwards to the real method. The exact own-method ABI symbol was obtained from `nm --defined-only`; it avoids hardcoded RMM/cuDF version namespaces. The fixed toolchain's CUDA stream ABI is still explicit in the private link-wrap symbol. Fault cases retain operation/ordinal/transport captures for precise failures.

## Control validation and pipeline expectation correction

The parent rebuilt with the corrected own-boundary wrapper and reported all 835 assertions in 7 isolated CUDA fault cases passing (`/tmp/sirius-s4-current-fault-tests-2.log`). This validates the selected native errors and pending-work drains for the control schedule, including HOST copies. Candidate constructor/ingress validation remains pending its compiled schedule run.

The first control normal suite reported 94 cases, 92 passed/2 failed, 6866 assertions with 3 failures (`/tmp/sirius-s4-current-tests.log`). Both failures were old pipeline expectations invalidated by bounded HOST fallback: the task test refused only the larger direct scratch, so HOST publication succeeded; the builder retirement test asserted that every request had the old direct size, causing a nested Catch failure on the smaller fallback request.

The existing task test now covers both refusal policies through its existing reservation-limit observer. It counts one direct request and one smaller HOST request, requires no rescheduling, four repository deposits, zero reserved memory after retirement, and worker reuse in both cases. Refusing both requests requires no filter, one resource skip, and allocation recovery after output retirement. Refusing only direct scratch requires two published filters, one HOST publication, no admission skip, and membership for every input row. The existing builder test refuses every allocation and checks the exact ordered `(device, bytes)` requests after `finish` returns, followed by unchanged allocation/reservation baselines. The observer remains test-side; production OOM policy is unchanged. Parent rebuild/rerun is pending for this patch.

Additional parent-reported edge fixtures passed: one GPU with forced HOST uses no workspace; two GPUs with only the root contributing use no root scratch and one 2 MiB HOST block; empty two-GPU direct input passes; SQL q4 reports `profile_verified=true` and `gpu_intercepted=true` without requiring an external environment guard. These are correctness checks, not evidence for a four-GPU schedule winner.

Source handoff: production and tests frozen after the pipeline patch; the parent may format selected files and build paired schedules. No production change was required by the normal-suite expectation failures or these edge checks.

## Final production cleanup, 2026-10-23

This section records new work only. Earlier implementation, recovery, and experiment entries remain historical. Final cleanup is in `/home/ubuntu/sirius/build/s4-final`, branch `stacked/df/04-final`, starting at `d925db5dcb909411598b57c566fd9888471d151c`. The parent owns builds, GPU execution, comparison fixtures/harness integration, and publication. This developer performed no build, GPU test, staging, commit, or push.

Reading refresh before edits: developer role, worktree AGENTS and Super Sirius README; Document Code and its cuDF reference; module-context; RMM CUDA Streams and Resource Refs; cuCascade fixed HOST allocation documentation and the actual event owner implementation at `1a928109`; memory-management Memory Reservations, reservation adaptor, Caller reservations for HOST conversions, Pinned Host Memory, and Stream-Ordering Discipline; query-lifecycle ownership/retirement; dynamic-filters Publication and consumption, Multi-partition build accumulation, Ordering and correctness, Configuration and Limitations. Personal substantive book sections listed above were reused; Core Guidelines I.11/F.16/R.1/E.6/Con.2 were refreshed. Read the recorded S4 validation, publication/recovery record, four-GPU screen results and the architect's finalization contract. No whole-book rereading or new runtime result is implied.

The four-GPU screen in `S4_EXPERIMENT_RESULTS.md` is external evidence: larger builder cases slowed 48.0-54.9% under concurrent ingress, and equivalent Q7/Q21 publication intervals slowed about 47%. The architect approved pruning that pilot while retaining serial direct publication and required bounded HOST fallback. These screening results do not establish S4-versus-prerequisite non-regression or satisfy the final all22 guard.

Removed the private publication-schedule CMake cache/definition, the concurrent-ingress branches, root ingress stream owners, per-source copied events, registration method, redundant allocation-ready event, compiled schedule getter, and measurement schedule field. Direct ingress still uses the task publication stream that allocates scratch; root OR waits on the completed ingress event, egress waits on OR, and half reuse follows OR across keys. The drain list now contains exactly the task stream and every partial stream. All workspace/partial owners remain alive while draining; failed joins still retain reachable owners. HOST workspace bounds, fallback timing, transfer accounting, native failure propagation, and task reservation policy are unchanged.

Newton approved keeping the existing three-argument `finish` and detail-only `finish_for_experiment(..., publication_transport, publication_measurement*)` signatures. The latter remains useful for deterministic direct/HOST correctness and failure tests; no public setting or replacement selector was introduced. Publisher calls and counters therefore need no signature migration. Native fixture metadata owns revision identity after the production getter is removed. The baseline comparison uses a test-only common/SQL overlay against untouched prerequisite production; final-only builder/transport checks exercise exact oracles and bounded HOST behavior. No cross-revision builder adapter or baseline production backport is part of this change.

The isolated fault regression retains early and late preparation failures. With the allocation-ready event removed, six successful constructions precede the last target-completion event: four half-buffer events, root completion, and source readiness. The late injected creation failure therefore moves from skip 7 to skip 6. Delayed root-merge completion moves from recorded-event skip 3 to skip 2 because source readiness and ingress completion now precede it. Exact native cause, accepted-work drain, unchanged partial bytes on preparation refusal, allocation recovery, and the complete byte oracle remain asserted. The existing normal accumulation, HOST exhaustion/reservation, and pipeline OOM tests remain applicable.

Static verification: reviewed the full owned diff, formatted only changed C++ line ranges with the existing clang-format, ran `git diff --check`, and searched production for the removed selector/getter/concurrent-ingress symbols. No unaffected comments or historical experiment artifacts were intentionally changed. Owned source is compile-ready; final build/test/sanitizer outcomes remain pending parent execution.

Parent-coordinated focused commands from the final worktree, once its build is ready:

```bash
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_unittest '[s4_publication],[in_task_publication]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_cuda_fault_test --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_unittest '[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]' --order decl
```

These commands are a handoff, not completed runs. Final direct/HOST correctness, bounded sanitizer evidence, cleanup parity against frozen S4 control, and matched prerequisite all22/non-regression validation remain parent-coordinated acceptance work.
