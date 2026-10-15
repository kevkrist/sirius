# TPC-H: does issuing siblings before A help?

**Production policy: issue A first.** This prioritizes the requested batch, while still issuing its siblings before waiting for A's completion. The controlled microbenchmark establishes circumstances where either order can win; these four real TPC-H queries do not establish a general A-first speedup. The measured evidence and archived sources below are unchanged.

At measurement time production was sibling-first. The subsequent own-first implementation also preserves sibling issuance when A's issue fails or A is cancelled, with terminal-state checking after both issuance steps. The archived own-first variant checked that state before issuing siblings. Successful issuance order is identical; this report does not claim a new performance measurement of the revised failure path.

Here, **A is the deferred batch whose future is first waited on (`get()` or `wait()`)**, and siblings
are other open batches registered on the same CUDA stream. Both variants preserve
batching: A-first issues A, then siblings, then waits for A's completion. It does
not wait for A to finish before issuing siblings.

## Measured query performance

TPC-H SF100, unchanged Q1/Q6/Q14/Q19, original local parquet layout (six lineitem
files), one NVIDIA L40S. These are materialized query times from **uninstrumented**
executables. Each number is the median of six executions from two process blocks;
the fixed order was siblings, own, own, siblings. Fresh means Sirius cache reset
before each execution, not cold physical storage. Retained means reuse without a
reset. Initial context executions are excluded from these medians.

With explicit opportunistic readahead of eight scans:

| Query | Fresh siblings / A-first, ms | A-first change | Retained siblings / A-first, ms | A-first change |
| --- | ---: | ---: | ---: | ---: |
| Q1 | 3247.51 / 3233.82 | −0.42% | 742.19 / 740.66 | −0.21% |
| Q6 | 2921.43 / 2911.01 | −0.36% | 248.34 / 257.43 | +3.66% |
| Q14 | 4058.38 / 4041.69 | −0.41% | 346.97 / 349.13 | +0.62% |
| Q19 | 3975.15 / 4013.14 | +0.96% | 448.21 / 459.61 | +2.54% |

Positive change means A-first is slower. The sum of fresh query medians changes
by **−0.02%**, and retained medians by **+1.18%**. With the backend's default zero
readahead budget, the corresponding changes are **+0.16%** and **+0.34%**.
These sums are a secondary aggregate, not a simultaneous four-query runtime.

Small differences often reverse between the two paired blocks. Retained Q6 and
Q19 with readahead favored siblings-first in both pairs, but two independent
blocks per policy cannot establish general effects or tail latency. We did not
extend repetitions after seeing the results. Full medians, sample ranges, and
paired deltas are in [the tables](results/tables.md) and [raw timing samples](results/timing_samples.csv).

## How often was A blocked?

There are two different questions: was A's prefetch **pending when the first
future was waited on**, and did A subsequently enter the cache's blocking wait?
The first describes the situation the ordering policy sees. Sibling issuance can
take enough time for A's prefetch to finish, so it need not lead to a later wait.

Separate instrumented runs with readahead enabled measured these entry states
over three fresh-cache executions per query, using the then-production sibling-first ordering:

| Query | A pending / all first-wait groups | A pending / groups with siblings | A and a sibling pending | A pending alone |
| --- | ---: | ---: | ---: | ---: |
| Q1 | 48/54 (88.9%) | 12/15 (80.0%) | 12 | 0 |
| Q6 | 18/33 (54.5%) | 8/15 (53.3%) | 8 | 0 |
| Q14 | 20/33 (60.6%) | 9/15 (60.0%) | 9 | 0 |
| Q19 | 65/66 (98.5%) | 15/15 (100%) | 15 | 0 |

The denominator that matters for ordering is **groups with siblings**. Across
those 60 groups, 44 had both A and a sibling pending, one had only a sibling
pending, and 15 had neither pending. **None reproduced the microbenchmark's clean
case of blocked A with all siblings ready.** Actual own-role cache wait calls
across these fresh executions numbered 44, 11, 13, and 56 respectively; they are
call counts, not a second first-wait-group denominator.

With the **default zero local readahead budget**, both policies observed zero
pending states and zero prefetch waits in all 56 diagnostic query executions.
With readahead enabled, the 24 retained-cache diagnostic executions also observed
zero. Thus A-pending is common in these fresh scans *when readahead is enabled*,
but absent in the default and retained-cache cases tested. Instrumentation can
perturb races; these are observed frequencies, not universal workload rates.

## What actually makes A block?

The [cache wait predicate](../../../src/io/cache/prefetching_cache.cpp#L785)
requires all of the following:

1. A has a valid cache handle and a nonempty requested range.
2. A requested range overlaps a cache chunk marked `loading`.
3. That handle's prefetch producer is itself still in `loading` state.

The demand read then waits for **that handle's whole prefetch operation** to
complete, avoiding duplicate work. A cache miss alone is insufficient. A demand
fill, an unrelated handle's prefetch, a prefetch that has not yet claimed the
fill, or a completed prefetch does not satisfy this particular predicate.

The local backend's readahead budget defaults to zero. An explicit positive
budget enables this race. In the tested opportunistic strategy, non-scan work,
an empty task queue, or memory-wait/downgrade events can arm readahead. Once armed,
it can use the full configured budget. The demand reader may then catch up with
an unfinished prefetch. Slow storage, reactor queueing, and scheduling can affect
that window, but the counters do not attribute each pending observation to one
of these causes. A `loading` observation is not proof that a physical disk request
has already begun: the state is published before backend dispatch.

This explains the limited transfer from the microbenchmark. Siblings-first aims
to start other work before A's read can block. In these multi-source groups,
however, a sibling's read could encounter its own pending prefetch first. Both
prefetches had already claimed their fills independently of deferred issuance.
Changing the order therefore often changes **which existing operation is waited
on first**, rather than starting otherwise-idle storage work. It can still affect
copy overlap and when demand work is issued; the end-to-end measurements decide
whether that helps. The same CUDA stream also serializes its copies.

## Validation and scope

All **224 timing executions and 112 diagnostic executions** matched independent
DuckDB CPU results for the same SQL/files. Nonfloating values were exact; floating
values used `abs(a-b) <= 1e-8 + 1e-10 * max(abs(a), abs(b))`. All 48 processes exited
successfully. GPU fallback was disabled. Diagnostic events had zero drops or
unclaimed first waits, and actual cross-file sibling groups were observed.

Separate Nsight Systems captures for all four queries show CUDA kernels, memory
copies, and GPU scan/operator ranges. Quent traces cover both orderings. All 60
additional profiled query executions also matched the CPU oracle, bringing the
total to **396 validated GPU query results**. Profile elapsed times are excluded
from the performance comparison. Q1 and Q19 produced fewer Nsight capture files
than query executions, with roughly doubled activity consistent with combined
windows in one capture; capture ordinals
must not be treated as one-to-one iteration labels. The
[profile summary](results/profile_summary.json) records these attribution limits.

Timing binaries contain no diagnostic objects. Build hashes, generated order-only
diffs, link-map evidence, per-process commands/configuration, query outputs, and
counter events are retained under [results](results/CONTRACT.md). The
[independent reviewer](INDEPENDENT_REVIEW.md) recomputed correctness, medians,
paired deltas, and diagnostic counts from raw outputs and audited source/binary
identities. Agent reading receipts are in [README](README.md#assigned-reading)
and the review evidence.

Primary configuration: 64 GiB host cache, 1 MiB blocks, four reactors, unpinned
parquet, one GPU. An earlier 64 MiB-block calibration was separated before any
paired timing: its staging geometry limited device misses to one staging slot
per reactor and produced much slower scans. It is not included in the ordering
comparison. The [contract](results/CONTRACT.md) records this premeasurement amendment.

These conclusions cover four selected queries at SF100, this file layout and
hardware, and two local readahead settings. They do not establish all-TPC-H,
remote-storage, different-scale, or different-cache-geometry behavior. Preserve
batching and the current heuristic; reconsider ordering only for a deployment
that demonstrates a repeatable query-level benefit under its actual workload.
