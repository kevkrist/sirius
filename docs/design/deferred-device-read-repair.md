# Deferred device read repair and ordering experiment

Status: architect handoff, 2026-10-15. This specifies repairs to the uncommitted
implementation reviewed in `REVIEW.md`; it does not assert that implementation or
validation has finished.

## Required behavior

The returned future is the caller's evidence that its destination was accepted.
If submission throws before returning that future, no other waiter may later
submit that destination. A migrated future must issue with the submission's CUDA
device and explicit stream. Destroying a datasource must close all unissued
batches without allocating, including during exception unwinding. Every member
of a closed batch observes its shared terminal result; errors must not reopen it.

Keep the existing batching mechanism, weak registry ownership, and datasource
API. Separate issuance order (policy) from issuing one batch exactly once
(mechanism). No new executor, polymorphic hierarchy, public tuning knob, or
general transaction framework is needed.

## Submission transaction

Change the private enqueue operation to produce the already-constructed member
future, rather than publishing a slice and constructing its future afterwards.

For an existing candidate batch, construct its deferred future first, then call
the mutex-protected append. Appending the slice is the final fallible operation;
on success only noexcept moves/destruction and return remain. If a concurrent
waiter closed the batch, discard the unused deferred future and retry another
candidate/new batch. Destroying an unused deferred future must not issue work.
The slice vector's strong exception guarantee protects failed growth.

For a new batch, all these preparation steps must finish before another waiter
can reach it: reserve capacity for its datasource pending-list slot, construct
the batch, construct the deferred future, and append its initial slice. Registry
registration is then the commit point: on successful registration, append the
weak owner-tracking entry to the already-reserved pending list and return the
future through nonthrowing moves. Hold the datasource pending mutex across this
transaction. A registry allocation failure happens before its weak entry is
published and leaves only locally owned state to unwind. The registry's vector
push must retain its strong exception guarantee; an empty map bucket after
failure is harmless.

This intentionally permits **pending mutex -> registry mutex** during new-batch
publication. It is acyclic because registry operations never take a batch or
pending mutex and release their mutex before issuing anything. Pending -> batch
already exists. A batch never takes pending or registry while holding its own
mutex, and issuance never nests two batch mutexes. Update comments that currently
forbid all registry nesting. Do not release pending between publishing the weak
owner entry and registration: another enqueue could obtain a future and make a
subsequent registration exception expose the original failed caller's slice.

Avoid a rollback-after-publication design: rollback cannot retract a request
already issued by a concurrent waiter. Avoid swallowing registration failures
as the primary solution; complete preparation before the commit point instead.

## Batch terminal state and teardown

Represent terminal errors/cancellation directly in batch state, independently of
whether a shared completion future exists. A compact explicit state enum plus
the existing shared future and an `exception_ptr` is sufficient. States are
open, issued, failed, and cancelled. The batch mutex protects transitions and
the issue call. `is_open` and append consult state, not future validity alone.

Issuing an open batch stores the returned shared completion and closes it. If
issuing throws, store `current_exception()` and close it as failed. There must
be no promise construction, error-string formatting, or logging on this failure
transition. An exception pointer preserves the existing exception without a new
allocation. Surviving member waits rethrow it; they never retry issuance.

Cancellation locks the batch, changes an open batch to cancelled, and returns.
It has no heap allocation. An issued batch is left issued: taking the same mutex
waits out any in-progress owner access. A surviving future can construct the
existing `logic_error` message when observing cancellation; if that allocation
also fails it may report `bad_alloc`, but the batch remains cancelled and cannot
touch the owner. Destructor correctness must not depend on exception creation.

The datasource destructor swaps/moves its existing weak pending vector into a
local vector under the pending mutex, releases that lock, then locks each weak
reference and cancels it. No growing owning-reference vector, fresh exception,
promise, or potentially allocating diagnostic belongs on this path. This also
preserves cancellation of a batch already captured by a sibling waiter.

Registry extraction should move the existing vector of weak references out of
its map entry rather than allocate a vector of strong references. The caller
locks each weak reference and skips itself after releasing the registry mutex.
This makes the normal extraction path allocation-free and prevents an OOM in
`await()` from abandoning a still-open batch whose other member can later write
the failed waiter's destination. Drop-all-members semantics remain weak: expired
siblings need not be issued. An issue error in a sibling stays on that sibling;
the requested batch is still issued and waited.

## Device and stream identity

Capture the current CUDA device while accepting each explicit-stream batch,
before publication. Match a candidate's recorded device as well as its stream.
Wrap each call into the owner's actual read implementation in RMM's existing
`cuda_set_device_raii`; this applies to own and sibling issuance and restores the
waiter's previous device on success and failure. Shared completion waiting itself
does not require retaining that device guard.

Route null/default, legacy-default, and per-thread-default stream handles through
the existing eager cache path. Deferring a per-thread stream to another thread
changes its identity; a device guard cannot repair that. This is an eligibility
rule for the optimization, not an extra caller restriction. Update documentation
and tests that currently expect these handles to defer within a datasource.
Capturing a device is a small CUDA runtime query, so replace claims that eligible
submission does literally no CUDA work with the accurate claim that cache
lookup, copies, and backend I/O are deferred.

## Why issue siblings first?

Three files A, B, C can queue reads before the consumer waits on A. Issuing only A
would keep B and C idle until their own futures are consumed. Issuing all three
on the first wait allows their disk or network work to overlap.

The less obvious choice is B, C, A rather than A, B, C. In this implementation
`device_read_ranges_async` can synchronously wait for an already-running
prefetch, inside `await_inflight_prefetch`. If A is waiting for prefetch, B and C
can start earlier when issued first. But if B is blocked, issuing B first delays
A; finite backend capacity can also make A wait behind B/C. Consequently this
is a heuristic, not a correctness requirement or a universally faster order.

The code also waits for all sibling *issue calls* to return before returning A's
wait, even if A's backend work completed earlier. Therefore first-future wait
latency and the instant A's actual I/O completes are distinct metrics.

## Benchmark specification

Own an isolated `bench/deferred-device-read-order/` program, runner, and results
document. Exercise actual datasource futures, cache arbitration,
`deferred_device_batch::await`, and registry extraction. Do not reimplement the
policy in a simulator and report its expected arithmetic as a measured result.

Use a bounded backend with two service workers and a finite queue, servicing real
prepared requests. A practical implementation can reuse the controlled backend's
queue behind workers that perform `pread` from local fixture files and the real
host/device copies before settling coordinator credits. Preserve cache chunk
callbacks and completion ordering. Alternatively wrap the real local I/O backend
with narrowly scoped benchmark observation/gating. Create deterministic file
contents and verify the resulting device buffers. A bounded latency gate may
model a prefetch already in flight, but must be labelled injected latency; actual
file read and device transfer work must still occur. Do not use an unbounded
thread per request.

Build the repaired production policy as the baseline. Build a second executable
using a temporary copy of `deferred_device_batch.cpp` where only `await` ordering
changes to own issue -> sibling issues -> own completion wait. An optional third
variant omits sibling issue and waits only its own batch. A checked transformation
must fail if the expected source anchors do not match; retain generated sources,
diffs/hashes, build commands, and binary identity in results. Do not edit the live
source repeatedly or introduce a production configuration option.

Queue A/B/C on one explicit stream, with multiple slices per source. Consume
their futures in A/B/C order on one consumer. Start service workers before the
timed section. Record request submission and completion timestamps by source,
not just final elapsed time. For a batch satisfied by a prefetch, distinguish
prefetch I/O from the later cache-to-device completion. Record A's actual read
completion using a completion observer or CUDA event after its copy, and record
when A's `wait`/`get` returns. Keep the observation method identical in variants.

Cover these cases:

| Scenario | Setup | What it checks |
| --- | --- | --- |
| No blocking prefetch | All three sources miss | Effect of finite queue capacity and reordered priority |
| Waited source blocked | A has in-flight prefetch; B/C miss | Whether B/C start and finish during A's prefetch wait |
| Sibling blocked | B has in-flight prefetch; A/C miss | Head-of-line delay imposed by sibling-first |
| Capacity sensitivity | Repeat with one and two workers | Whether overlap survives backend saturation |

Use at least two read sizes (for example 64 KiB and 4 MiB per source), several
slices per source, warmups, and repeated paired/alternating variant runs. Keep
file-cache state and injected delays identical. Report median and p95 first-wait
latency, A-I/O completion latency, all-complete latency, actual issue order, and
overlap evidence. Record GPU/backend/environment, worker count, byte volume,
iterations, cache regime, delays, and whether disk reads were page-cache warm.
Timings are performance evidence, not pass/fail thresholds in unit tests.

Keep the existing policy initially while gathering evidence. Retain it if the
blocked-A overlap is observable and meaningful, documenting blocked-sibling and
capacity costs. Change it only with evidence that the alternative improves the
representative objective; a constructed adversarial case alone cannot establish
which prefetch position is common in production. Do not claim a TPC-H/query-level
speedup from this microbenchmark. If neither order dominates, state the tradeoff
and leave the heuristic explicit rather than adding a tuning knob.

## Validation and ownership handoff

The core developer owns the two datasource/batch implementations and headers,
their focused regressions, affected build wiring, and existing scan docs. The
benchmark developer owns only the benchmark directory and its results/runner;
coordinate any CMake edits through the core developer. Both preserve all existing
uncommitted work and read their assigned role materials before implementation.

Required regressions are: allocation sweep while adding to a retained batch;
new-batch failure through pending capacity and registry insertion with another
waiter; migrated explicit-stream wait from GPU 1 on a GPU 0 thread (including
device restoration and real data); eager default-stream submission including a
per-thread-default resident copy; allocation-free datasource teardown with live
members under sustained OOM; issue failure under sustained OOM without terminate
or retry; cancellation concurrent with sibling issuance; and the existing
backend-error, dropped-member, cache-shutdown, and concurrent-waiter suite.

Reuse `test/cpp/utils/host_allocation_fault.hpp` and the existing preload fault
library instead of adding production allocation hooks. Failure sweeps should
observe destinations sent to the backend, retain a successful first member, and
keep all buffers alive while checking stale registration. Isolate terminate/death
probes in subprocesses. Tests must not rely on fixed allocation ordinals alone.
For an OOM on first `get`, verify a second member receives the same terminal
failure and no later issuance occurs. Run a sustained-failure variant as well as
single-allocation failure, so a second allocation in a catch cannot escape.

The reviewer owns `REVIEW.md`, reads the prescribed reviewer books/guidelines and
this specification, checks actual code rather than relying on the design, and
independently reruns the focused tests and selected benchmark cases. In particular
audit every operation after both publication commit points for throwability, the
lock graph, and the nonallocating registry/cancellation/issue-failure transitions.

## Architect reading receipt and references

Read `/home/ubuntu/.codex/agents/architect.toml`, repository `AGENTS.md`,
`REVIEW.md`, Super Sirius README, full architecture overview and execution flow,
scan overview/manager lifecycle/I/O read path/backend/cache seam, and the memory
document's pinned-memory and stream-ordering sections. Inspected current
datasource/batch source and headers, cache device read arbitration, and deferred
tests including teardown, concurrent issue, defaults, sibling issuance, and
shutdown. The parent searched for `/module-context` and reported it unavailable;
local dependency source is the fallback before implementation.

Book reading was selective, not a claim of reading whole books:

- `A Philosophy of Software Design.md`: contents, sections 4.1–4.4 and 5.1–5.2
  (local lines 1047–1235 and 1423–1526). Applied deep interfaces and information
  hiding to keep lifetime/state mechanics inside the existing batch abstraction.
- `C++ Software Design.md`: contents; Guideline 4's motivation and separation/
  testability discussion (1888–1938 and 2150–2249); Guideline 6's behavioral
  abstraction discussion, pre/postcondition discussion, and conclusion
  (2641–2765, 2800–2855, 3020–3068). Applied failure and migrated-future behavior
  as API contracts rather than imposing new caller restrictions.
- `C++20 -- The Complete Guide.md`: contents; section 7.1.2 range algorithm
  constraints (6450–6520), sections 10.2 and introductory 10.3/10.3.1 span lifetime
  and performance discussion (12090–12240), and 19.4.1 atomic shared-pointer
  discussion/example (23985–24080). Keep borrowed slices behind owned batch
  lifetime and mutex synchronization; reuse ranges/concept-constrained backend
  interfaces instead of introducing a new inheritance hierarchy.

Reference implementations consulted:

- Local DuckDB `src/storage/standard_buffer_manager.cpp`, `Prefetch` and `Pin`
  (264–335): grouping bulk reads is separate from protected pin ownership; its
  comments also identify destructor-driven reentrant lock hazards.
- [Velox BufferedInput](https://github.com/facebookincubator/velox/blob/main/velox/dwio/common/BufferedInput.cpp),
  `enqueue` and `load`: collected regions are sorted/merged before vector reads;
  this supports keeping collection separate from execution, not a particular
  first-wait priority policy.
- [DataFusion stream wrappers](https://github.com/apache/datafusion/blob/main/datafusion/physical-plan/src/stream.rs),
  `ReceiverStreamBuilder` and blocking-task drop behavior: owned task lifetime,
  cancellation, and completion are explicit. Already-running blocking work is
  not forcibly terminated; Sirius likewise must wait out owner access before
  destruction. Neither reference supplies evidence for sibling-first GPU I/O.

## Decision addendum: requested batch first

After the microbenchmark and TPC-H SF100 comparison, the user selected the simpler requested-batch-first policy. This supersedes the provisional siblings-first recommendation above. The [TPC-H results](../../bench/deferred-device-read-order/tpch/RESULTS.md) remain evidence from the two measured policies, not a claim that either ordering is universally faster.

`deferred_device_batch::await` now calls its own `issue_if_open()` before extracting and issuing sibling batches, then checks its own terminal state and waits for its shared completion. Own issuance releases its mutex before registry extraction or sibling issuance. No completion wait or own terminal-error throw moves ahead of sibling issuance: a failed or cancelled requested batch still dispatches live siblings. Concurrent callers may interleave; the policy specifies each waiter's attempted issue order, not a global scheduler order.

The ordering regression registers the sibling first and waits on the later registered batch, checks backend submission order, and verifies both requests are submitted before either completes. Completing the requested batch alone releases its waiter while the sibling remains incomplete. The parked-sibling teardown regression verifies that the requested batch is submitted and completed before a sibling's prefetch wait ends, while the waiter and sibling destructor remain blocked until sibling issuance can finish. Separate cancellation and allocation-failed issuance cases preserve sibling dispatch after own terminal failure. Existing concurrency, migration and allocation-failure regressions remain applicable.

Developer reading for this addendum preceded edits: `/home/ubuntu/.codex/agents/developer.toml`, repository `AGENTS.md`, Super Sirius README, architecture overview, execution flow, scan I/O/read-path/cache sections, memory-management pinned-memory/stream-ordering sections, the repair design and `REVIEW.md`. Selective book reading: *C++ Software Design* G4 testability/separation (1888-1938, 2150-2249), G6 abstraction introduction (2641-2690); *C++20 -- The Complete Guide* span lifetime/design sections (12090-12170); Core CP.20-22, CP.42-44, CP.50 and E.16. Applied existing public test seams, scoped non-nested locks, borrowed destination lifetime, and allocation-free terminal transitions. Also read the [official cuDF documentation guide](https://github.com/rapidsai/cudf/blob/main/cpp/doxygen/developer_guide/DOCUMENTATION.md), local cuDF `datasource::device_read_async` contract and RMM `cuda_set_device_raii` contract. `/module-context` was unavailable in the parent session; those local dependency headers supplied the relevant contracts.
