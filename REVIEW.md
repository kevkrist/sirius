# Review addendum: requested batch first

**No blocking findings. Focused runtime tests and allocation-fault regressions pass.** The user explicitly selected A-first for its clearer priority semantics after reviewing the measured tradeoff. That decision supersedes the historical recommendation to retain siblings-first; the earlier evidence and review are preserved below.

## Current change

[`await`](src/io/deferred_device_batch.cpp#L63) first calls its own `issue_if_open()`, then extracts/issues registered siblings, then checks its own terminal state and waits for its completion. Own issuance releases its mutex before registry extraction or sibling issuance. A failed or cancelled A still dispatches live siblings before reporting its error. This specifies each waiter's attempted order; concurrent waiters can interleave.

Source audit found no change to publication exception guarantees, device restoration, weak ownership, allocation-free cancellation, issue-once behavior or terminal failure storage. No nested batch lock was introduced. Independent comparison confirms that implementation from `issue_if_open()` onward is unchanged from the reviewed repair snapshot.

The revised regressions distinguish requested priority from registration order by waiting on the last-registered batch, obtain both backend requests before completing either, and verify that completing A releases its waiter while the sibling backend remains incomplete. The parked-sibling test verifies A reaches/completes at the backend before the sibling's blocking prefetch ends, while the first waiter and sibling destructor remain blocked until issuance finishes. New cancellation and sustained-OOM terminal-failure cases verify sibling dispatch still happens before A's error is reported. The existing concurrent-waiter, migrated-device and allocation-failure coverage remains relevant.

## Benchmark and documentation audit

The shared [checked generator](bench/deferred-device-read-order/prepare.py) now names both policies explicitly, irrespective of which is production. Both the CMake microbenchmark and [full-SQL builder](bench/deferred-device-read-order/tpch/build_variants.py) link their named policy object explicitly. Independent CPU-only checks verified that production own-first is preserved byte-for-byte, transforming in both directions round-trips exactly, and the alternative moves only own issuance while leaving terminal checking after both issuance steps.

Independent SHA-256 comparison confirmed all **83 historical files** under the two `results/` trees unchanged. Updated live documentation distinguishes the current user-selected policy from the measured sibling-first baseline. It also discloses that the historical own-first experiment checked terminal errors before siblings, whereas current production preserves sibling dispatch on own failure/cancellation. No new performance claim or timing matrix is inferred from this change.

## Validation and assigned reading

Parent build passed (`/tmp/deferred-own-first-build.log`), as did scoped pre-commit checks (`/tmp/deferred-own-first-core-lint.log`, `/tmp/deferred-own-first-benchmark-lint.log`). Reviewer independently ran the freshly built binary through Pixi, under the shared GPU lock after checking for foreign GPU processes:

- `[deferred_device_reads],[read-arbitration],[scan_manager][config]`: **63 cases / 520 assertions passed**; `/tmp/deferred-own-first-review-focused.log`.
- `[deferred_host_allocation_fault]` with `libsirius_host_allocation_fault.so` preloaded: **4 cases / 124 assertions passed**; `/tmp/deferred-own-first-review-fault.log`.

Both physical GPUs 0 and 1 were visible for the migrated-wait regression. Parent ran six named-policy benchmark smokes: no blocked source, blocked A and blocked B under both policies, each with two measured iterations plus one warmup. All verified device bytes and exited zero. Reviewer inspected `/tmp/deferred-own-first-smoke/validation.json`, logs and no-block traces: own-first enqueues A/B/C and sibling-first enqueues B/C/A. Historical artifact hashes were checked again afterward: all 83 remain unchanged. No new performance matrix, full-suite runtime or sanitizer run was performed for this narrowly scoped policy change.

Reviewer reused the completed role/book/engine reading recorded in the historical receipt below: both reviewer role files; Core R.1/R.20–24, CP.20–22/42–44/50, E.16; *C++ Software Design* G4/G6; C++20 span/atomic ownership and joining-thread sections. Before conclusions, re-read the full repair design and its [decision/reading addendum](docs/design/deferred-device-read-repair.md#decision-addendum-requested-batch-first), current await/state/registry code, revised tests, scan/API documentation, generators/CMake wiring, and live benchmark narratives. Core developer's new receipt is in that design addendum; benchmark developer's reused/additional receipt is in its README. No claim of reading entire books is made. Reviewer changed only `REVIEW.md`, ran the coordinated tests above, performed no build, and preserved other agents' edits.

---

# Historical review: deferred device read repairs and ordering benchmark

**No blocking findings. All three original defects are closed.** The current uncommitted production changes, regressions, build wiring, benchmark and runner were inspected, repaired tests were independently rerun, and the completed measurement matrix was independently checked.

The reviewer changed only this report, made no commits, and preserved external `review-artifacts/` files. Original findings/reproducer evidence: `/tmp/deferred-review-repro/initial-REVIEW.md`. Reviewed tracked-diff SHA-256: `5a08a029985593f69e4157ee95b6c71e439c363ece38f2af80338c868fc1b75c`. Untracked benchmark and design files were inspected separately.

## Original findings: disposition

| Finding | Required property and repair | Result |
| --- | --- | --- |
| P1: failed future construction left a destination available for later writes | A failed submission cannot publish its borrowed destination. [Existing-batch submission](src/io/sirius_datasource.cpp#L242) constructs the future before the protected append. [New-batch submission](src/io/sirius_datasource.cpp#L252) reserves owner tracking and prepares future/slice before committing registry registration; only nonthrowing bookkeeping/return remain. | **Closed.** Allocation sweeps for existing and newly registered batches pass; rejected destinations never reach the backend. |
| P2: migrated future used the waiter's CUDA device | Preserve original explicit stream/device independently of the waiter. [Submission](src/io/sirius_datasource.cpp#L230) captures the device; [issuance](src/io/deferred_device_batch.cpp#L104) uses an RMM device guard. [Default/shared handles](src/io/sirius_datasource.cpp#L208) use the eager path. | **Closed.** GPU 1 submission/GPU 0 waiter test passes with an actual transfer, byte comparison and restored waiter device. Default-stream and per-thread-default resident-copy tests pass. |
| P2: allocation during destruction caused termination | Invalidate unissued batches without allocation or later owner dereference. [Destruction](src/io/sirius_datasource.cpp#L90) swaps the existing weak container; [cancellation](src/io/deferred_device_batch.cpp#L95) only changes protected state. Exceptions are constructed when a surviving future observes cancellation. | **Closed.** Sustained-OOM teardown performs zero intercepted allocations, cancels both futures and issues no I/O. |

These remedies establish the original properties without a general transaction framework or production policy knob. Excluding default streams from deferral is appropriate: a device guard cannot reconstruct another thread's per-thread default stream.

## Correctness and design audit

- **Publication:** Inspected every operation after both commit points. Existing-batch slice append has the strong exception guarantee and happens after future construction. New-batch registry insertion can fail only before publishing its weak entry; reserved pending-list insertion, ownership bookkeeping, future moves and lock release after success do not allocate or throw.
- **Terminal errors:** Explicit open/issued/failed/cancelled state prevents reopening. Issue failure stores the existing `exception_ptr` without creating a replacement promise. Sustained-OOM first-get coverage confirms that another member receives the terminal failure with no allocation or reissue.
- **Lock graph:** Pending can acquire batch or registry. Registry takes neither pending nor batch and releases its lock before issuance. Issue holds only its own batch mutex. No cycle or nested batch issuance was found. Cancellation waits out any issue still using the owner.
- **Lifetime:** Registry extraction moves the existing weak vector without allocation. Abandoned batches expire. A sibling retained by a waiter checks cancellation under the same batch mutex before accessing its datasource. Existing cache/backend ownership carries completion once issuance returns.
- **Documentation/C++20:** Updated API and scan docs cover borrowed destinations, migrated waits, eager defaults, terminal cancellation and ordering tradeoffs. Span/ranges/concepts and owned joining workers fit their purposes. No separate design, guideline or style blocker was found.

## Benchmark audit

Inspected the [actual benchmark](test/io/deferred_device_order_benchmark.cpp), [generator](bench/deferred-device-read-order/prepare.py), [runner](bench/deferred-device-read-order/run.py), [CMake wiring](cmake/sirius-tests.cmake), and [methodology](bench/deferred-device-read-order/README.md).

Production datasource futures, cache arbitration, batch issuance and registry extraction feed one/two persistent workers doing real `pread` and H2D. Staging/workers initialize before timing. The workload submits three requests, with an eight-request queue ceiling. Every device buffer is downloaded and byte-verified, including warmups. Injected latency and warm OS page-cache state are disclosed.

The generated alternative only moves the sibling block within `await`: own issue, sibling issues, own completion wait. It does not wait for own completion before issuing siblings or edit live production policy. The source manifest matched the reviewed implementation: baseline hash `633104d513bd8f7258bceaddba5aa11f3665461401ed1aa967a1c2d794e27266`; alternative `6c25432a1a3ba1910012d19cfcbd3781f1b64b3a4fd7c20eb6ec8608a7ba3caf`.

The same copy wrapper observes bytes in both variants. Its nonallocating callback makes no CUDA calls. It measures observed stream completion including callback scheduling overhead, not hardware GPU time. A-data-ready, A-get-return and all-futures-return are separate metrics.

**Independent smoke:** Blocked A/B × siblings/own-first, two workers, 64 KiB/source, two measured iterations plus one warmup, 20 ms gate and 2 ms service delay: all four executions verified every byte. The blocked-A siblings-first trace shows B/C completing before A's prefetch completes. These runs overlapped a CPU build and establish correctness/trace behavior only. Logs and traces: `/tmp/deferred-repair-review/blocked_{a,b}-{siblings_first,own_first}/` and adjacent logs; exact commands: `/tmp/deferred-repair-review/benchmark-smoke.py`.

The explanation is sound: issuing all sources at the first wait enables overlap; siblings-first specifically helps when own issuance blocks on prefetch. A blocked sibling or saturated backend can favor another order. Neither the rationale nor this controlled workload establishes production blocked-source frequencies or a TPC-H speedup.

**Completed matrix:** The parent ran the full matrix without concurrent build/GPU work. The reviewer independently recomputed all 48 median/p95 summary rows from 960 measured samples (20 per policy/configuration), verified that all 96 process logs report byte verification, checked both binary hashes against metadata, and inspected representative source-order traces. The final [results interpretation](bench/deferred-device-read-order/RESULTS.md) matches those measurements and accurately limits the claims. [Retained samples](bench/deferred-device-read-order/results/samples.csv), [summaries](bench/deferred-device-read-order/results/summary.csv), [traces](bench/deferred-device-read-order/results/trace.csv), and [validation record](bench/deferred-device-read-order/results/validation.txt) make the evidence reviewable; complete process logs remain in `/tmp/deferred-order-results/`.

For two workers, 4 MiB per source, and 2 ms injected service latency, median all-complete time with blocked A was **23.120 ms siblings-first versus 25.809 ms own-first**. With blocked B, A-data-ready was **25.811 versus 2.945 ms**, while A-get-return was **25.834 versus 22.810 ms** and all-complete **25.921 versus 25.730 ms**. Without blocked prefetch, all-complete was similar (**5.815 versus 5.870 ms**), though own-first delivered A sooner. The traces confirm the mechanism: siblings-first lets B/C finish during A's prefetch; own-first lets A finish during B's prefetch. This supports retaining the existing policy as an explicit heuristic with costs, not claiming either order universally wins.

## Validation

Parent-reported checks:

- Full current unit-test/helper build and both benchmark targets succeeded.
- `[deferred_device_reads],[read-arbitration],[scan_manager][config]`: **62 cases / 507 assertions passed**.
- Isolated fault filter: **3 cases / 110 assertions passed**.
- Scoped pre-commit checks passed (`/tmp/deferred-repair-precommit-final.log`); `git diff --check` also independently passed.

Reviewer independently executed the current binary:

```bash
pixi run build/release/test/cpp/sirius_unittest '[deferred_device_reads]'
pixi run env LD_PRELOAD=/data/sirius_optblind/wt/landing-f1-deferred-vectored-reads/build/release/libsirius_host_allocation_fault.so build/release/test/cpp/sirius_unittest '[deferred_host_allocation_fault]'
```

- Deferred suite: **26 cases / 305 assertions passed**, including the actual multi-GPU transfer/restoration. Log: `/tmp/deferred-repair-review/deferred-tests.log`.
- Fault suite: **3 cases / 110 assertions passed**, covering allocation sweeps, sustained issue failure and allocation-free cancellation. Log: `/tmp/deferred-repair-review/fault-tests.log`. The test-only preload helper is inactive in ordinary runs.
- Four benchmark smoke executions passed as described above.

**Limits:** No full-suite execution, sanitizer run or end-to-end SQL benchmark by the reviewer. Allocation sweeps start the waiter after faulted submission; simultaneous allocation-failure/issuance interleavings are covered by source audit, not a deterministic concurrent fault test. Backend OOM outside this change was not exhaustively injected. Benchmark conclusions are restricted to its disclosed workload/environment.

## Assigned reading receipts

Large books were read selectively, not claimed read in full. Reading preceded conclusions.

**Architect:** Detailed [receipt](docs/design/deferred-device-read-repair.md#architect-reading-receipt-and-references) records role/repository/engine documentation; *A Philosophy of Software Design* 4.1–4.4/5.1–5.2; *C++ Software Design* Guidelines 4/6; C++20 ranges/span/atomic-shared-pointer sections; DuckDB, Velox and DataFusion reference implementations.

**Core developer:** Receipt delivered to the parent and reviewed here: role/repository/engine docs and repair specification; *C++ Software Design* G4/G6; C++20 span lifetime/atomic-shared-pointer sections; Core R.1/R.20–24, CP.20–22/42–44/50, E.16; official cuDF documentation guide and local RMM/cuDF headers. The parent searched for `/module-context` and reported it unavailable; local dependency source supplied implementation contracts.

**Benchmark developer:** Detailed [receipt](bench/deferred-device-read-order/README.md#developer-assigned-reading-receipt) records role/design/engine/dependency docs; *C++ Software Design* G4/G9; C++20 chapter 18 joining-thread sections; Core C.36/C.37, E.1–4, CP.20/22/42; official cuDF documentation guide.

**Reviewer:** Reused the original completed reading: both `/home/ubuntu/.codex/agents/reviewer.toml` and `/home/ubuntu/AgentDocs/codex-agents/reviewer.toml`; repository AGENTS and Super Sirius architecture/execution flow, relevant scan/cache/configuration/memory and stream-ordering docs; Core resource overview, R.1/R.20–24, CP.20–22/42–44/50, E.16; full *C++ Software Design* G4/G6 after contents; C++20 10.2, introductory 10.3, and 19.4.1 after relevant contents. For this repair pass additionally read the full [repair specification](docs/design/deferred-device-read-repair.md), benchmark methodology/receipt, revised source/tests, local RMM device-guard contract, and C++20 18.1/18.3 joining-thread sections (book lines 21819–22006 and 22600–22684). Source audit and independent execution, rather than design/reading receipts alone, establish the findings above.
