# VERIFY_transfer: do the microbenchmark conclusions transfer to TPC-H SF1000?

2026-11-18. Adversarial check of BASELINE_SUITE / PROFILE_SUITE / BASE_REFERENCE / MB_BLOOM / MB_LIFECYCLE.
Scripts + raw output: `$R/runs/verify_transfer/` (`kern.py`, `kern_sum.py`, `probe_sum.py`, `probe_split.py`, `topk.py`, `dups.py`,
`c_gap*.txt`, `r_gap*.txt`).

## 0. What I ran

- **Re-read of existing nsys captures (no GPU):** `CUPTI_ACTIVITY_KIND_KERNEL` of the 10 new tip captures (`runs/profile/nsys`) and
  7 reused ones (`runs/base/nsys_tip`): every `add_if_n` (insert) and `membership_probe_functor` (probe) kernel, with device,
  thread count (= rows, 1 thread per row), duration. Insert numbers = last hot iteration; probe numbers = mean of the 3 iterations
  of a capture (iter0 included; kernel work is the same in each).
- **New microbench cases, under `flock machine.lock`**, existing binary `wt-mb-bloom/.../df_bloom_bench` via the MB agent's
  `run_cases.sh` (GPU 0, one process per case, median of >= 6 batches / 6 reps): insert and probe at 24..280 MiB (13 sizes, the
  range the MB sweep skipped), 8 bits/key at 42/55/87/140 MiB, int32 vs int64 at 36/57/83/109 MiB with 30M-row batches. 44 cases,
  0 failures, each once. My 280 MiB insert (6.86 G rows/s) and 24 MiB (44.8) agree with MB_BLOOM's 300 MiB (6.75-6.84) and 16 MiB
  (45.6). No Quent session created (binary loads no Sirius config).
- **Duplicate counts of Bloom build keys:** DuckDB (CPU, Python, under the lock, 1.7 s) on `tpch_parquet_sf100`; row counts x10
  match the SF1000 `valid` rows of the log pass where the build side was identified.
- Not run: no new TPC-H timing, no new nsys, no prototype. Nothing written to the root disk; `$R/repo` untouched.

## 1. Shape match: TPC-H SF1000 vs microbench sweeps

| Dimension | TPC-H SF1000 (log pass + nsys) | Microbench | Match |
|---|---|---|---|
| Bloom array | 20 keys: 4, 21, 23, 36, 57 (x5), 59, 83 (x2), 85, 87, 109 (x2), 133, 174, 278, 281, **1394** MiB. 17 of 20 keys (and 56% of the 3188 MiB) lie strictly between 16 and 300 MiB | MB1/MB6: 16 MiB, (150), 300 MiB, 1.5 / 3 / 12 GiB | **gap**: nothing sampled where TPC-H lives; only q21 j18 is in the "flat" regime |
| Rows per batch | 0.06M .. 147M (q12 0.3-0.7M x44; q4 2-6M; q3 32-38M; q21 144-147M) | 1M / 16M / 128M | ok |
| Batches per build | 1-10, 36, 44 | 1..512 | ok |
| GPUs | 4, one partial per GPU, kernels run concurrently on 4 devices | MB1/MB6: 1 GPU; lifecycle G=1/2/4 | ok for kernels (per-GPU rate identical, section 2) |
| Key type | insert: int32 and int64 (`convert_to_rep<int>` / `<long>`); probe: `int->int`, `int->long`, `long->long` adapters | insert both; probe int64 only | probe partly uncovered |
| Nulls | none (`valid == rows` on all key lines) | 0 and 12.5% | ok |
| Duplicates (build) | x1.00-1.07 everywhere checked, except q9 j29: x4.0 (ps_partkey) / x4.4 (ps_suppkey) | x1, x10 | x10 case does not occur |
| Probe key order | lineitem / orders probes are clustered (same key in adjacent rows) | unique random (and sorted unique) | **not covered**, changes probe rate 2.3-3x (section 3) |
| Finish transport | 3 sources / 3 targets (14 of 19), HOST staged | PEER only at G=2: 1 target, 1-2 sources | 4-GPU PEER not measurable here (section 5) |
| Exact hash IN-list size | 63K .. 2.0M keys | MB5: 1e3, 1e5 (1e7 becomes Bloom) | 0.4-2M range not sampled; production 0.9-3.2 ms per join, ~1.6 ms per 1M keys |

Duplicate check (SF100, x10 = SF1000 rows): q21 j18 = orders with `o_orderstatus='F'`: 73.07M rows, 73.07M distinct (x1.00;
730.8M at SF1000 matches). q21 j33: x1.05. q12 j8: 3.115M / 2.923M (x1.07). q9 j29: 4.352M rows, 1.088M distinct partkey, 0.989M
distinct suppkey. q7 j35 and q10 j21: my guess of the build side did not reproduce the logged row count, **not identified**.

## 2. Insert (MB1 -> TPC-H): production path and rate

Production kernel is the benchmarked one: `cuco::detail::bloom_filter_ns::add_if_n<false, 8, 256, ...>`, 1 thread per row, same
grid shape as MB_BLOOM's nsys. Per-site rate, last hot iteration, against the microbench at the same array size (gap run):

| site | array MiB | rows/batch | prod G rows/s | microbench G rows/s | prod / MB |
|---|---|---|---|---|---|
| q21 j18 | 1394 | 144-147M | 5.93 | 5.86-5.89 (1.5 GiB) | 1.01 |
| q3 j15 | 281 | 32-38M | 7.00 | 6.86 | 1.02 |
| q7 j35 | 278 | 36-39M | 8.05 | 6.86 | 1.17 |
| q8 j41 | 174 | 15-30M | 8.61 | 8.23 | 1.05 |
| q21 j33 | 133 | 17.4M | 11.4 | 10.3 | 1.11 |
| q4 j9, q10 j10 | 109 | 2-9M | 15.3-15.7 | 13.2-14.3 | 1.1-1.2 |
| q5 j32, q10 j21, q9 j29 | 83-87 | 4-15M | 23.5-25.7 | 17-22 | 1.1-1.5 |
| q12 j8 | 59 | 0.3-0.7M | 38-40 | ~29 | 1.3 |
| q3 j8, q5 j25, q8 j34, q16 | 57 | 3-30M | 50.4-51.0 | 30-39 | 1.3-1.7 |
| q22, q7 j38, q9 j19, q20 | 4-36 | 0.06-12M | 46-51 | 40-47 | 1.0-1.25 |

Gap curve, microbench, unique random int64, 16M batches (G rows/s): 24 MiB 44.8; 36: 40.3; 48: 37.0; 57: 30.3; 64: 24.5; 72: 20.9;
83: 17.8; 96: 15.4; 109: 13.2; 133: 10.3; 174: 8.2; 224: 7.3; 280: 6.9. int32 is 5-13% faster in the knee; a 30M batch at 57 MiB
gives 37-39 instead of 30.

- Out of cache the transfer is exact (q21: 1%). In the 36-110 MiB knee production is 1.1-1.7x **faster** than the microbench
  (cause not identified; the microbench is conservative).
- q21 j18 placement read out (PROFILE left it open): 5 batches on 4 GPUs, one GPU runs two inserts back to back in every hot
  iteration (24.8 + 24.2 = 49.0 ms; device 2, 3, 0 in the three iterations). Critical-path insert in q21 is **49 ms**, not 26;
  pre-finish 56.2 ms vs DF-off site 12.1 ms = **44 ms net (3.1% of q21)**. About 24 ms of it is the fifth batch landing on a busy
  GPU (batch placement, not a kernel property).
- Suite critical-path insert, per-device sums: 22.5 ms for all other sites + 49 ms = **~69 ms (0.45% of 15372 ms)**; PROFILE's
  46 ms was a lower bound. Still under the 0.5% suite gate.
- Engine insert / scatter ratio is 3.5x (q21 j18: 24.7 vs 7.0 ms), 0.6x (q7 j35: 4.4-4.8 vs 7.5), 0.3x (q10): MB_BLOOM's
  "2.0-2.2x" is one point of a wide range.

## 3. Probe (MB6 -> TPC-H): not measured by the profile agent, and the model does not transfer

Production probe kernel: `cub for_each ... membership_probe_functor<integral_probe_adapter<K,F>, bloom_lookup | set_lookup |
needle_lookup>`, launched from the compressed-scan decode (`simpatico::decompress_scan_filter`, `compressed_scan.cpp:445`), not
from `compute_mask`. Same functor family as MB6, different call site and inputs.

Probe kernel time per suite pass (17 DF queries; per-device sum = upper bound on wall, kernels on one device may overlap):

| kind | rows probed | GPU ms (sum over 4 GPUs) | per-device ms | % of suite |
|---|---|---|---|---|
| Bloom (`bloom_lookup`) | 25.3 B | 500 | <= 132 | <= 0.86 |
| exact hash IN-list (`set_lookup`) | 23.6 B | 475 | <= 123 | <= 0.80 |
| small IN-list (`needle_lookup`) | 0.4 B | 2.4 | 1.4 | 0.01 |
| insert (`add_if_n`), for scale | 1.7 B | 198 | 53-69 | 0.35-0.45 |

Per query, Bloom probe per-device ms: q21 22.4, q9 20.5, q3 20.0, q4 18.2 (5.6% of q4), q5 16.8, q10 13.7, q22 5.4, q7 5.2,
q12 3.7, q8 3.1, q16 2.6. Exact-set probe: q8 23.2, q21 22.7, q17 21.8 (4.4% of q17), q18 15.0, q20 12.0, q7 10.8, q19 10.4, q11 5.9.
**Probe kernels cost about 4-5x the insert kernels**; no ranking row covers them (PROFILE only has tip-minus-DF-off scan
pipelines, which net cost against benefit).

Production Bloom probe rate against MB6 (gap run: 24 MiB 105; 57: 93.5; 83: 86; 96: 83; 109: 61; 133: 30; 174: 21; 280: 16.8;
1.5 GiB 14.3 G rows/s):

| filter | array MiB | prod G rows/s | MB6 at that size | prod / MB6 |
|---|---|---|---|---|
| q21 j18 (+j33) on lineitem | 1394 | 33.5-42.5 | 14.3 | 2.3-3.0 |
| q3 j15, q7 j35 on lineitem | 278-281 | 41-47 | 16.8 | 2.4-2.8 |
| q4 j9, q10 | 85-109 | 39-58 | 61-86 | 0.6-0.95 |
| q5 j32 | 87 | 48 (long->long) .. 75 (int->long) | 85 | 0.56-0.88 |
| q9 j29, two filters | 2 x 83 | 47-53 | 86 single / 21-30 at 166 | 0.6 / 1.8-2.4 |
| q12 | 59 | 53 | 92 | 0.58 |
| q3 j8, q5 j25, q8 j34 (int->int) | 57 | 78-88 | 93.5 | 0.85-0.95 |
| q16, q9 j19 | 21-57 | 118-121 | 94-105 | 1.1-1.3 |

- Production rates sit in a 34-120 G rows/s band; MB6 predicts 14-105. Array-size sensitivity in production is ~2.3x from 57 MiB
  to 1.4 GiB, MB6 says 6.5x. Likely cause: clustered probe keys (about 4 adjacent lineitem rows per order hit the same sector);
  MB6 only used unique keys. Inferred, not verified (the bench has no duplicate option for probe keys).
- `long->long` probes plateau at 50-60 G rows/s even for in-cache arrays (q12 59 MiB: 53; q22 36 MiB: 60), so halving a
  85-110 MiB array buys <= 20% there, not MB6's +50%.
- Exact-set probes run at 34-36 G rows/s in q8, q17, q21, q11 and 64-76 in q7, q18, q19, q20 (cause not identified); an in-cache
  Bloom probes at 80-120. New candidate, unmeasured: kind choice for 0.2-2M-key builds (plan policy, #2569). Upper bound if all
  exact-set probes ran at ~100 G rows/s: ~60 ms per-device (0.4% suite); q17 ~14 ms (2.9%), q8 ~15 ms (1.6%).

## 4. Lifecycle (MB_LIFECYCLE -> TPC-H)

| item | microbench | production (nsys, tip) | verdict |
|---|---|---|---|
| contribute host time | 4.7-6.1 us (10-11.5 us under nsys) | 36-88 us per contribution under nsys (q21 37, q3 36, q10 43, q12 50, q4 88) | number off by 3.5-8x; still <= 2.2 ms summed over threads (q12) |
| API calls per contribution | 1 kernel, 1 event, 2 wait-event, 1 get-device | 8 calls incl. `cuKernelGetName` | same |
| begin | 30-700 us (Bloom, G=4, session level) | 0.2-0.4 ms per construction, one 1.84 ms | same |
| small IN-list publish | 329 us (G=4) | 0.23-0.28 ms, 10 syncs | same |
| hash IN-list join, G=4 | 1.0 ms at 1e5 rows, 1 batch | 0.9 ms @0.4M, 1.5 @0.8M, 2.4 @1.3M, 3.2 @2.0M keys | row-proportional part not sampled by MB5; ~18 ms suite on the critical path, needed at base too |
| allocator / reservation | test fixture, no task reservation | no reservation wait, retry or downgrade observed (Quent) | caveat does not bite |

HOST classification check: q21 finish copies total 15.3 GB = 10 x 1527 MiB = (2S+1+T) x bytes with S=T=3. A peer path would move
(S+T) x bytes = 9.2 GB. So the "PtoP" label CUPTI puts on 12 GB is a labelling artefact; the HOST-staged attribution holds.

## 5. Verdicts on the key findings

| # | Finding | Verdict | Evidence / corrected number |
|---|---|---|---|
| 1 | Suite: DF worth 1.08x; tip total 15372 ms | confirmed (arithmetic only) | per-query medians re-summed: 15371 ms; 89% of the +1255 ms comes from q4, q10, q7, q9, q21. Not re-timed |
| 2 | MB1 insert rates transfer (5.8 G rows/s out of cache) | confirmed | q21 5.93 vs 5.86-5.89; all 16 sites within 1.0-1.7x, production never slower |
| 3 | MB1 "8 bits/key: -26% at 300->150 MiB, 0-1% above" as the TPC-H answer | weakened | 19 of 20 arrays are <= 281 MiB; in the knee halving cuts insert time 30-59% (109->55 MiB: 13.2 -> 32.5 G rows/s). Absolute: ~8.5 ms per suite (0.06%) |
| 4 | H04 critical-path insert: 46 ms suite, q21 26-44 ms | weakened (too low) | q21 = 49 ms insert on one GPU, 44 ms net (3.1%); suite ~69 ms (0.45%) |
| 5 | H04 levers killed: second stream, plain add, atomics/layout, sort | confirmed | production kernel = benchmarked kernel, no nulls, same launch shape. Nothing contradicts |
| 6 | "Read-first insert alive for duplicate-heavy builds" | refuted for TPC-H | only q9 j29 has duplicates (x4, 83 MiB arrays, 1.1 ms per-device insert): gain <= 0.6 ms. q21 j18 is unique `o_orderkey` (BASELINE's "l_orderkey from lineitem" guess is wrong), where read-first costs 0 to +4% (up to +2 ms) |
| 7 | H02a "no saving" | weakened | true for the domain cap. With true distinct counts q9 j29 shrinks 166 -> ~40 MiB (4% of suite Bloom bytes); nothing else |
| 8 | MB6 probe model (14 G rows/s out of cache, +49% from halving 300 MiB) transfers | refuted quantitatively | production is 2.3-3.0x faster on 280 MiB-1.4 GiB arrays and 0.6-0.95x on 60-110 MiB ones; same functor, different call site and key order |
| 9 | Ranking covers the DF kernel costs | weakened | probe kernels missing: Bloom <= 132 ms, exact-set <= 123 ms per-device per suite pass (<= 1.66% together) vs insert 69 ms |
| 10 | DF construction without the bounce = 45 ms (0.30%) | confirmed within +-0.1 pp | 52 ms from positive remainders, ~60 ms with q21 at 44 (0.34-0.39%) |
| 11 | Lifecycle fixed costs are small (H03/H06/H07/H08/H14 killed) | confirmed | production values match MB at G=4 except contribute host time (3.5-8x higher under nsys, still negligible) |
| 12 | PEER model: q21 ~30 ms, q3 6.6 ms, suite 64-70 ms (0.4%) | weakened | measured with 1 target at G=2 (20 ms/GB, ~50 GB/s = one link). With 3 targets behind the root's link the same data gives up to 3x: q21 ~85-92 ms (6%), q3 ~17-19 ms, suite ~190-210 ms (1.2-1.4%). Neither figure is verifiable on this machine |
| 13 | H05 probe cost q9 +36.6 ms, suite ~67 ms | weakened (upper bound) | q9 Bloom probe kernels are 20.5 ms per-device for both keys; the 98.9%-keep key probed ~1.57 B rows before the gate cut it: ~8 ms. q8 j41's 174 MiB Bloom is never probed (q8 Bloom probes are all the 57 MiB filter): its whole cost is build side |
| 14 | HOST bounce = 600 ms (3.9%), every finish copy is staging | confirmed | byte count matches the 10-leg model exactly |

## 6. Expected suite-level gains, recomputed (suite 15372 ms; per-query noise +-3% or +-10 ms)

"Excl. bounce" = what the stack's own mechanics can move. "This machine" adds the HOST-loop time the same change removes
(excluded mechanism, reported only).

| Variant | Excl. bounce, ms per suite (%) | This machine incl. bounce | Largest per-query effect | Realistic? |
|---|---|---|---|---|
| Second stream / plain add / atomics / layout | 0 to 0.5 | same | - | no gain |
| Read-first insert | +0.6 (q9) to -2 (q21) | same | - | no gain on TPC-H |
| Balance q21's fifth batch (not a DF change) | <= 24 (0.16%) | same | q21 1.7% | outside the stack |
| 8 bits/key, insert | ~8.5 (0.06%) | +300 (1.95%) | q8 1.8 ms, q3 1.6 ms | below noise |
| 8 bits/key, probe (gross, before 25x false positives) | <= ~15 (0.1%) | - | q9 ~9 ms, q4 ~3 ms | sign unknown: ~0.75 B extra false-positive rows pass the scans per suite pass (3.2% of the ~23 B non-matching probed rows) |
| Distinct-count sizing (q9 j29 only) | <= ~13 (0.08%), mostly probe | +24 | q9 0.9% (2.6% with bounce) | needs a distinct estimate at build |
| Skip useless filters: q9 key 2, q8 j41 | 12-22 (0.08-0.14%) | +16 (q9) +35 (q8) | q8 4.1% and q9 ~2% with bounce; 0.4% / ~1% without | q8 j41 is the stack's +36 ms regression vs base |
| Fixed costs: single wait, lazy lease, shared event, coalesced slices, single-GPU fast path | ~10, at most 25 (0.07-0.16%) | same | q2 ~2 ms (1.5%, inside noise) | below noise |
| Larger chunks | 0-8 on a P2P machine | +24-30 | - | excluded here |
| Halve Bloom bytes on a 4-GPU P2P machine (model) | 32-105 (0.2-0.7%) | - | q21 15-45 ms | unverifiable here |

- Sum of every non-excluded construction lever with a non-negative expected value (fixed costs, useless filters, q9 sizing,
  8-bit insert): **~25-60 ms, 0.15-0.4% of the suite**; no
  single one reaches the 0.5% suite gate, and only q9 (useless key + sizing, ~1-2%) and q21 (batch placement) reach the 1% query
  gate. None is resolvable in suite timings at the measured noise; each needs nsys / Quent attribution to prove.
- The costs that are large are outside the allowed scope or outside construction: HOST bounce 600 ms (3.9%), stack regression vs
  base +147 ms (1.0%, q3 +35 and q8 +36, both the bounce now on the critical path), probe kernels <= 255 ms per-device (1.66%).

## 7. Unverified / limits of this check

- Probe per-device sums are upper bounds on wall time; whether the GPU bounds the scan pipelines was not checked.
- Cause of clustered-probe speedup, of the two exact-set probe rates (35 vs 72 G rows/s), and of production's faster in-cache
  insert: not identified.
- q7 j35 and q10 j21 build sides not identified, so their duplicate ratio is unknown (q10 j21 85 MiB, q7 j35 278 MiB).
- 8 bits/key downstream false-positive cost not measured anywhere; the "sign unknown" stands until a suite run at 8 bits.
- 4-GPU PEER behaviour (parallel vs shared link) is an argument from link bandwidth, not a measurement.
- Gap microbench: one process per case, no repeat (MB agent's A/A noise 0.1-0.3%); GPU started from idle clocks (180 MHz reading).
- Probe averages include the warm iteration of each capture; q11, q16-q20, q22 come from the base agent's tip captures only.
