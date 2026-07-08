Track A / #1014 acceptance report (SF=300, GPU2)

**Verdict: PASS.** `dynamic_filter_build_priority=off` has no material wall-time or resident-peak regression versus `legacy` outside legacy's own observed spread. It is faster on all six workloads (median improvement 9.2%–25.3%), result bags are equivalent across all three configurations, and legacy/off coverage aggregates are identical.

Machine: 4× NVIDIA GB200 (189,471 MiB each), driver 580.105.08; this run was serialized on physical GPU2 via `CUDA_VISIBLE_DEVICES=2`. Dataset: existing `/tmp/sirius_tpch_parquet_sf300`. Code measured: A1 `1d47577c`, A2 production code `5557c151` (final A2 is `8ea96bc2`; the only post-measurement amendment was Black formatting in the Python runner). Final A3: `73e8f3fd`.

Protocol: three independent processes, INFO timing runs with 8 iterations/query and iteration 0 discarded; separate one-iteration DEBUG processes excluded from timing. Workload: TPC-H Q5/Q7/Q8/Q9/Q21 plus the checked-in eight-join `many_join` query. Times below are QueryBegin→QueryEnd medians. “Spread” is legacy max−min over the retained seven runs. Peak is median INFO `[dynf_summary] high_water` GPU bytes. Coverage is aggregate rows entering channels before/after publication.

| Query | No filter: median s / GPU GiB | Legacy: median s / spread s / GPU GiB | Off: median s / delta / GPU GiB | Coverage legacy→off (pre/post rows in) |
|---|---:|---:|---:|---:|
| q5 | 3.994 / 49.97 | 4.722 / 0.857 / 15.76 | 3.528 / -25.3% / 15.76 | 0/1,913,259,365 → 0/1,913,259,365 |
| q7 | 4.164 / 19.35 | 5.266 / 0.591 / 17.39 | 4.253 / -19.2% / 17.39 | 0/594,860,669 → 0/594,860,669 |
| q8 | 6.693 / 61.63 | 6.175 / 1.515 / 64.23 | 5.518 / -10.6% / 64.23 | 0/181,717,190 → 0/181,717,190 |
| q9 | 6.007 / 69.74 | 6.130 / 1.248 / 19.45 | 5.395 / -12.0% / 19.45 | 0/1,799,989,091 → 0/1,799,989,091 |
| q21 | 7.491 / 27.59 | 8.384 / 2.444 / 27.59 | 7.145 / -14.8% / 27.59 | 0/1,802,989,091 → 0/1,802,989,091 |
| many_join | 6.560 / 58.19 | 5.373 / 1.066 / 15.76 | 4.879 / -9.2% / 15.76 | 0/2,153,258,011 → 0/2,153,258,011 |

Peak comparison: legacy→off changes range from -0.0034% to +0.0016%; every query passes the `off <= legacy median + legacy spread` criterion. Some no-filter and later-query peak rows have `exact=0`, so those are lower-bound/process-peak observations as designed; the decision comparison is config 2 versus config 3, where values are essentially identical. Feeder `running_end=0` for every retained query. Under `off`, median prioritized feeder dispatches are zero for every query; under legacy they range from 11 to 333. Replica high-water and coverage match between legacy/off.

Correctness: one unique canonical result SHA-256 per query across all configs and all iterations. DEBUG analyzer: 6 analytical queries/config, shape 1.7, zero FormatWarnings in all three processes.

Artifacts on the performance host:

- Raw runs, manifests, logs, result bags, and analyzer outputs: `/tmp/sirius-track-a/measurement/`
- Aggregate CSV/JSON: `/tmp/sirius-track-a/measurement/summary.csv`, `summary.json`
- A1 baseline evidence: `/tmp/sirius-track-a/`

A3 therefore flips the default to `off`. Rollback remains available for one release with `SET dynamic_filter_build_priority='legacy'` or YAML.
