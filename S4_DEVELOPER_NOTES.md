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
