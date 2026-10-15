# Measured deferred read ordering results

Follow-up: [real TPC-H SF100 query measurements](tpch/RESULTS.md) test how often
these blocking scenarios occur and whether changing the ordering helps queries.

These historical measurements were taken before the subsequent production switch to own-first, chosen by the user. Archived sources, results, and hashes are unchanged. The current implementation also preserves sibling issuance after an own-batch failure; that failure-path adjustment was not part of this timing experiment.

The benchmark demonstrates why sibling-first existed, and why it can also delay the requested file. With A blocked on prefetch and two backend workers, sibling-first completed the whole three-file workload earlier. With no prefetch block, it delayed A while leaving overall completion similar. A blocked sibling also delayed A. These results document the scheduling tradeoff; they do not establish which order is best for production query workloads.

Measured on 2026-10-15 using logical GPU 0 of an NVIDIA L40S system, driver 615.71.09, Linux x86_64. Both executables were built from the repaired uncommitted implementation. The suite completed 96 subprocesses, 960 measured iterations, and 192 warmups. Every iteration verified every byte of all three device destinations. Each configuration has 20 measured samples per policy in two rounds with reversed policy order. See [methodology and commands](README.md), [environment and executable hashes](results/metadata.json), and [validation record](results/validation.txt).

## What the measurements show

The following medians use 4 MiB per source, four slices per source, and two backend workers. Each cell is **sibling-first / own-first**, in milliseconds. "A ready" is the observed completion of all A copies; "A get" is the return of the first future; "all get" is the return after consuming all futures.

| Scenario | Injected service delay per request | A ready | A get | All get |
| --- | ---: | ---: | ---: | ---: |
| No prefetch block | 0 ms | 1.726 / 0.941 | 1.743 / 0.962 | 1.753 / 1.747 |
| A prefetch blocked | 0 ms | 20.960 / 20.943 | 21.054 / 20.987 | 21.062 / 21.779 |
| B prefetch blocked | 0 ms | 21.673 / 0.890 | 21.704 / 20.735 | 21.797 / 21.655 |
| No prefetch block | 2 ms | 5.787 / 3.018 | 5.807 / 3.037 | 5.815 / 5.870 |
| A prefetch blocked | 2 ms | 23.018 / 22.982 | 23.110 / 23.053 | 23.120 / 25.809 |
| B prefetch blocked | 2 ms | 25.811 / 2.945 | 25.834 / 22.810 | 25.921 / 25.730 |

The prefetch gate is 20 ms in both blocked scenarios, in addition to any service delay. File reads and host-to-device transfers are real; both delays are injected. The 2 ms case makes the overlap easier to see without claiming that this is the machine's disk or network latency.

The p95 values for the three 2 ms cases are:

| Scenario | A get p95, sibling / own | All get p95, sibling / own |
| --- | ---: | ---: |
| No prefetch block | 6.100 / 3.613 ms | 6.107 / 6.452 ms |
| A prefetch blocked | 23.150 / 23.118 ms | 23.159 / 26.201 ms |
| B prefetch blocked | 27.509 / 22.869 ms | 27.519 / 25.820 ms |

With only one worker, blocked-A overlap largely disappeared: for 4 MiB/source and 2 ms service, median all-get was 28.559 / 28.645 ms. The prefetch gate occupied the sole worker. This is why merely enqueuing siblings does not guarantee useful parallel work.

## Direct evidence of the policy

The no-block trace recorded backend enqueue order B, C, A for sibling-first and A, B, C for own-first. In the first 4 MiB/two-worker/2 ms sample, sibling-first enqueued those requests at 7.86, 16.15, and 41.04 microseconds after timing began; own-first enqueued A, B, and C at 7.62, 16.76, and 31.16 microseconds.

For blocked A in that configuration, B and C finished before A's prefetch in **all 20 sibling-first samples**, and in **none of the 20 own-first samples**. One recorded sibling-first sample completed B at 2.979 ms and C at 5.882 ms while A's prefetch finished at 22.704 ms. The paired own-first sample completed A's prefetch at 22.707 ms, then B at 25.711 ms and C at 25.793 ms. Starting siblings before entering A's blocking issue call exposed real overlap.

For blocked B, own-first made A ready much earlier (2.945 ms median), but A's first `get()` still returned at 22.810 ms. That call issued A and then blocked while issuing B before it could return. Data readiness and future-return latency are different measurements; conflating them would overstate own-first's user-visible gain.

## Evidence and limits

[All raw samples](results/samples.csv), [all request traces](results/trace.csv), and [all 48 median/p95 summaries](results/summary.csv) retain both read sizes, worker counts, and service delays. [Exact commands](results/commands.json), the [source transformation](results/generated/own-first.diff), and [source hashes](results/generated/manifest.json) identify the comparison. The generated source snapshots are retained alongside those files. Complete subprocess logs remain in `/tmp/deferred-order-results`.

This is a controlled, page-cache-warm scheduling microbenchmark with a simple bounded backend. Its per-slice synchronization and identical completion-observer callbacks contribute overhead. It measures neither the throughput of the production io_uring backend nor a TPC-H speedup. The subsequent TPC-H investigation measures blocked-source frequency in real scans. The production switch to own-first is an explicit policy choice, not evidence of a universal speedup; no runtime tuning option is added.
