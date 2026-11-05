# Verify (causality lens): rank 2, driver stream-ordered allocator lock

**Verdict: UNDERSTATED.** The mechanism is causal and I found no alternative that fits the data. The 1.80 s share is the
conservative end of the range. Corrected central estimate: **≈2.3 s of g4 wall (range 1.8-2.8), about 21% of the
11.08 s gap.** Two wording corrections are listed below.

Scripts and outputs are in `30-diagnosis/verify_alloc_causality/`. They are CPU only; no GPU work was run.
- `phase_state.py`: per-thread state inside one phase.
- `phase_cf.py`: per-phase counterfactual.
- `suite_phase_cf.py` → `suite_phase_cf.out`, `suite_phase_cf_lincap.out`: suite counterfactual with link, kernel and
  per-phase linear caps.

## 1. Code path (verified)

- Sirius installs no GPU MR factory: no `set_gpu_memory_resource_factory` or pool/arena MR anywhere in `src/`.
- So `memory_space.cpp:122-127` takes the default path, `rmm::mr::cuda_async_memory_resource(capacity)` (or
  `common.cpp:551-556` when the configurator supplies the default fn).
- `reservation_aware_resource_adaptor.cpp:462-475` `do_allocate_unmanaged` then calls `_upstream.allocate` directly.
  There is no caching layer.
- **Alternative ruled out: pool trim churn.**
  - rmm's default `release_threshold` is UINT64_MAX (`rmm/mr/cuda_async_memory_resource.hpp:79-81`), so memory is not
    released and re-mapped at syncs.
  - The microbench also sets threshold = max and still reproduces the contention. So the contention is intrinsic to
    the driver call, not caused by remapping.
- **Alternative ruled out: a per-context lock.**
  - Microbench 1 GPU × 16 threads: mean 37.7 µs at 385k/s.
  - Microbench 4 GPU × 4 threads: mean 55.9 µs at 279k/s.
  - With a per-context lock, 4×4 would look like 1×4 (10.9 µs). It does not, so the lock is process-wide, and more
    contexts make it worse.
- **Alternative ruled out: other driver calls holding the lock.**
  - `cudaLaunchKernel` latency is flat (6.8 → 7.8 µs).
  - Latency vs in-flight allocator callers follows the same curve at g1 and g4 (X1 E3).
  - A pure alloc/free microbench reproduces the magnitude un-profiled.

## 2. Discriminating checks

### Thread state inside Q18 PARTITION(5) (`phase_state.py`, nsys2 iter2)

| | g1 | g4 |
|---|---|---|
| Phase (profiled) | 577 ms | 474 ms |
| Tasks in flight | 3.85 of 4 | 15.5 of 16 |
| Allocator calls | 79,002 | 79,002 (identical) |
| Allocator thread-ms | 335 (15% of task time) | **5,405 (74%)** |
| Mean threads inside the allocator | 0.58 | **11.4** |
| Kernel busy per GPU | 188 ms | 45-48 ms (about 10% of phase) |

- Threads are neither starved nor waiting on the GPU. They are queued in the driver allocator.
- Rival "single-threaded global serializer" (creator, repository mutex), which would also explain why the phase is flat
  from g2 to g4 (433/316/313/324 ms): **refuted**, because all 16 threads hold tasks.
- Counterfactual (allocator calls at g1 per-call cost): phase 166 ms profiled, about 113 ms un-profiled (ratio 0.68).
  g1/4 is 108 ms, so this phase returns to linear.

### Do the experiment correlations discriminate? Per-query r, 22 queries

| Predictor | batch4g ΔT4 | thr8 g4 regression |
|---|---|---|
| X1 allocator price | **0.96** | **0.97** |
| Quent task count | 0.70 | 0.53 |
| GPU-origin (clone) prepare | 0.46 | 0.52 |
| Total task thread-ms | 0.69 | 0.59 |
| Per-query gap | 0.80 | 0.65 |

- Allocator price clearly beats the competing per-task and per-fragment predictors. These are not just "big queries
  move more" correlations.
- Nuance on thr8: a pure throughput cap predicts about 0 change at 8 threads per GPU. The microbench aggregate actually
  rises, 279k → 300k/s. The measured +1.19 s therefore comes from convoy and write-lock collateral, e.g. memcpy issue
  rdlock waits 0.46 → 4.6 thread-s in X1.
- That is still this lock, but "throughput caps at 280-385k/s" alone does not explain thr8.

## 3. Price under critical-path and floor constraints (`suite_phase_cf.py`)

**Method.**
- Unit: each pipeline phase of each query, g4 iter2.
- Excess: allocator calls inside that pipeline's task ranges, minus the g1 suite per-call mean (8.0/2.9 µs). Convert to
  wall by dividing by the mean tasks in flight.
- Floors on the counterfactual phase:
  - max per-GPU kernel busy;
  - H2D bytes / 110 GB/s;
  - the per-phase gap to linear, g4 span − g1 span/4 (Quent un-profiled, iters 2-4; X1 did not apply this one).
- Scaling: by the phase's own un-profiled/profiled span ratio (0.68-0.84 on the allocator-bound phases), then by
  union/sum to handle overlapping phases.

**Result: suite 3.75 s profiled → 2.75 s un-profiled → 2.58 s after removing overlap.**

| Query | Saving (ms) |
|---|---|
| Q18 | 494 |
| Q1 | 312 |
| Q21 | 244 |
| Q12 | 214 |
| Q9 | 187 |
| Q4 | 168 |
| Q8 | 163 |
| Q10 | 139 |
| Q5 | 137 |
| Q15 | 133 |
| Q6 | 105 |
| Q14 | 93 |
| Q19 | 62 |
| Q3 | 58 |
| Link-bound and small queries | ≤11 each |

- **The linear cap matters.** Without it the estimate is 2.79 s. Q1 drops from 473 to 312 ms: the naive E/n
  counterfactual would beat g1/4, because freed threads would then contend for SMs (g1 Q1 kernel-busy is 0.92).
- **Remaining upward biases, about 0.2-0.4 s:**
  - E/n assumes freed threads are fully productive.
  - Q6/Q19 cf lands about 40 ms under the true 105 GB/s floor, because a link-floored cf was scaled by the ratio.
- **Not counted:** memcpy-issue collateral, which would add a little.
- **Estimates compared:**

  | Source | Estimate | Method |
  |---|---|---|
  | Families | 1.70 s | Critical-path closure; Q1 priced at 254 ms because they counted only operator compute inflation, not decode or prepare allocator excess |
  | X1 | 2.4 s (1.8-2.9) | Bins with a link cap |
  | This check | 2.58 s | Per phase, with caps |

- **Adopted: 2.3 s (1.8-2.8).** The waterfall must still close. The extra ~0.5 s should come out of the buckets that
  absorbed allocator time in the families: FRAG task-end frees, RESID, and the Q1 "other" share.

## 4. Corrections to the claim text

- **"Biggest allocation source is the partition slice deep copy"** is overstated.
  - The PARTITION op class is the biggest *priced* class: 1.62 of 3.18 s profiled.
  - Within PARTITION, the slice deep copy (`table` leaf, `gpu_partition_impl.cpp:213-221`) has 165k mallocs and
    `cudf::hash_partition` internals (`:187`) have 147k. That is about half each.
  - Suite-wide by call count, `make_numeric_column` (342k) and `host_to_gpu` decode buffers (251k) are larger.
  - R2's ≈0.6 s is consistent with "half of PARTITION".
- **Affected queries:** Q19 is not about 0. It is 43 ms profiled (X1) and about 60 ms here, partly under the link.
  Q6 (≈70-105) and Q14 (≈93) belong in the list.
- **Overlap with rank 6 (forced P≥N partitioning):** PARTITION allocations double (146k → 294k) partly because
  exchanges that pass through at N=1 partition at N=4.
  - The excess-vs-g1-per-call method avoids double-counting the share.
  - The levers still overlap: removing forced partitioning removes part of this lock's price, and vice versa.

## 5. Checks of claim numbers (all reproduce)

| Claim | Status |
|---|---|
| Nested locks 63.3/78.1 thread-s (g4) vs 2.4/10.0 (g1) | Sums of X1 E2 rows match |
| Q18 PARTITION(5): 77.7k calls (79.0k in window), 4.2 vs 68.4 µs | Reproduced |
| thr8 +1.19 s | Reproduced: 21.23 − 20.04 |
| batch4g Δgap 2.43 s | Reproduced |
| r = 0.96 / 0.97 | Reproduced |
| Microbench 10.9 / 55.9 / 103.8 µs | Matches results.txt |
| X1 soft-cap profiled price | 3.18 s in the signed version; the positive-part `price_table.txt` sum is 3.48 s (g1 self-check of the positive part is +1.04, so 3.18 is the right one) |

## 6. Remedy sanity

- R1 (a per-GPU sub-allocator at `reservation_manager_configurator.hpp:103`) removes the lock. The microbench rules out
  a per-context fix, so it has to sit in user space.
- An rmm pool per GPU has one mutex per GPU with about 100 ns holds. Expect near-g1 behaviour.
- Its cross-stream reuse uses `cudaEventRecord`/`cudaStreamWaitEvent`, which X1 E4 shows are not coupled to the writer
  lock.
- ΔT4 ≈ 3.0 s is plausible, but its Δgap is bounded by the 2.3 s (1.8-2.8) above plus the g1 savings divided by 4.
