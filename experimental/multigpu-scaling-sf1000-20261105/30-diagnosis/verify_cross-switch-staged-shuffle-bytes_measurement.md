# Verify rank 4 "cross-switch staged shuffle bytes" (measurement lens)

Verdict: **CONFIRMED on size, corrected on mechanism and on how much is fixable.**
- Corrected wall share: **1.2 s** (range 0.85-1.3).
- About **0.85 s** of that is the cross-switch uplink floor, which is hardware/topology.
- Only **about 0.35-0.45 s** is the serialized-staging design defect that the title names.
- The "1.4 s after batch4g / 2.2 s at baseline" remedy value is mostly per-clone latency, i.e. rank #3, not this bucket.

Scripts and outputs are in `30-diagnosis/verify_bytes_meas/` (CPU only, re-runnable). Data: nsys2_g4, iter 2, each
query restricted to its own map window.
- `clones.py` → `g4_clones.csv`
- `stage_breakdown.py` → `g4_stage_breakdown.txt`
- `perclone.py` → `g4_perclone.csv` (50,497 clones)
- `perquery_bytes_share.txt`
- `overlap_dir.py` → `overlap_dir.txt`

## 1. Raw numbers re-derived independently

| Claim | Re-derived | OK? |
|---|---|---|
| 33,667 staged clones, 117.6 GB | 33,667, 117.6 GB | yes |
| 16,830 peer clones, 59.0 GB | 16,830, 59.0 GB | yes |
| Staged 21.7 thread-s, peer 3.55 | 22.23 / 3.82 (outer `convert:gpu_to_gpu` range; the claim used the inner range) | yes |
| 2:1 staged:peer (uniform matrix) | count ratio 2.000, byte ratio 1.99 | yes |
| Staging overlaps scan H2D for only 37 ms | 5.2 ms of staged-clone H2D on the same switch overlaps non-clone H2D | yes (even smaller) |
| Staged costs 3.6x the peer cost per byte | Per-clone regression of thread-ms on MB: staged 0.247 ms + 0.118 ms/MB, peer 0.120 + 0.0305 ms/MB. Marginal per-byte ratio 3.9x; fixed per clone 2.1x. Overall per clone 2.9x (3.6x was CONCAT only) | roughly |

**Profiler artifact check.**
- nsys clone thread-time is 26.05 s. Quent's un-profiled GPU-origin prepare is 25.45 s (`X2/v2/gprep.csv`, iters
  2-4). Q3 alone: 6,222 vs 6,061.
- So nsys/CUPTI does not inflate clones. Profiled→un-profiled for clone shares is about ×0.95, not ×0.75 or ×0.92.
- Iter-2 nsys matches the iter 2-4 Quent mean. There is no cold or warm issue.

## 2. Corrections to the evidence

**(a) "62-97% of the sync wait is the clone's own DMA (Q3 4,978 of 5,123 ms)" is mislabeled.**

Staged clone thread-time, 22,230 ms in total, splits like this (`stage_breakdown.py`):

| Part | Thread-ms | Share |
|---|---|---|
| Own memcpy actually executing | 5,968 | 27% |
| Issued, waiting while another copy in the same direction runs on that device (copy-engine/link contention) | 3,195 | 14% |
| Issued, not started, no other copy running | 6,463 | 29% |
| Host side, next copy not yet issued (sync return, alloc, loop) | 5,300 | 24% |
| Tail | 1,304 | 6% |

- For Q3, own DMA is 1,588 of 5,635 ms (28%).
- X2's "own DMA" means waiting until the clone's own copies *complete*, so it includes queueing and latency.
- The "issued, not started" part scales with bytes (0.042 ms/MB). It is per-1-MiB-chunk start latency: D2H 15.7 µs and
  H2D 10.1 µs per copy, over about 350k chunk copies. The D2H leg runs on the destination's stream under the source
  device context (`representation_converter_builtins.cpp:640-666`).

**(b) "Clone phases reach ~30 GB/s per switch" holds only for Q3 (27.3 / 27.4 GB/s).**
- Suite-wide, staged H2D per switch over the staged-clone union is 60.1 GB / 4.14 s = **14.5 GB/s**. Q18 14.6, Q21 13,
  Q10 14, Q9 20, Q13 21.
- While a staged copy is actually running, per-switch H2D is about 45 GB/s (2.66 s of busy union for 117.6 GB over 2
  switches), which is near the uplink.
- So the links sit idle about 2/3 of the staged-clone time. That is latency (per chunk and per clone), not leg overlap.

**(c) Overlapping the two legs alone is small.**
- Σ min(D2H busy, H2D busy) per staged clone = 2.72 thread-s → **0.39 s wall share profiled, ≈0.37 s un-profiled**.

## 3. Pricing the bucket: wall share (∫1/n_active), un-profiled ≈ ×0.95

The staged cost driven by bytes (own DMA + contention + chunk-start latency, excluding per-clone host/tail) is
1,908 ms profiled for the whole suite (`perquery_bytes_share.txt`).
- On the 11 queries the families price in BYTES (Q4/5/7/8/9/10/12/13/14/18/21): **1,351 → ≈1.28 s**. RANKED says
  1.23 s.
- Per-query agreement with the family critical-path prices:

  | Query | Family | Here (byte share) |
  |---|---|---|
  | Q9 | 242 | 238 |
  | Q21 | 261 | 237 |
  | Q13 | 190 | 170 |
  | Q14 | 70 | 82 |

- Q18 whole fan-in share is 476 vs the family's 452+50.
- Counterfactual "cross pairs cost the peer per-byte rate" (0.0056 ms/MB): excess 883 ms profiled on those queries
  (0.84 s), and 1.19 s suite-wide.
- Q3 alone has 215-366 ms of staged-byte share (27.2 GB). RANKED books it under #6 (JS-F2, Q3 broadcast policy), not
  #4, yet the claim lists Q3 as an affected query. That causes no double count, but the list is misleading.
- No double count with #1 (scan bytes only) or within the families (F4 = floor and F2 = remainder, measured additively).

**Split of the bucket (family queries).**
- Uplink floor = Σ_q max over switches of staged bytes in / rate: **832 ms at 52 GB/s, 962 at 45, 1,082 at 40**.
  Suite-wide it is 1.17-1.52 s.
- The floor is unavoidable for any cross-switch shuffle on this box: no cross-switch P2P, one uplink per switch, and
  no overlap with the scan today.
- What remains is the design or mechanical waste of serialized staging: 1,351 − floor = **0.27-0.52 s profiled,
  ≈0.25-0.5 s un-profiled**.

## 4. What this means for the ranked row

- `est_wall_s_g4` 1.23 is fine; use **1.2 (0.85-1.3)**.
- But kind should read **"topology/hardware ≈0.85 + design ≈0.4"**, not mainly design.
- "Legs serialized → 3.6x per byte" is not the main mechanism:
  - when staged bytes move, they move near link rate;
  - leg overlap is worth 0.37 s;
  - most of the excess is per-chunk and per-clone latency, which belongs with #3.
- **Remedy value.**
  - In this bucket, pipelining or batching can recover at most the ~0.4 s above the floor.
  - The 1.4 / 2.2 s figures come from removing per-clone latency, i.e. host 0.89 s plus tail 0.19 s profiled share,
    which is #3.
  - Fewer chunk copies (`cudaMemcpyBatchAsync` or larger chunks) attacks the 0.80 s share of chunk-start latency. That
    is already counted partly here and partly in #3.
- Only cutting cross-switch bytes can touch the ~0.85 s floor: orderkey co-location, broadcast, or a different
  topology. The claim's ≤0.5 s for co-location is plausible but unmeasured.
- Switch-aware placement = 0 holds (exact 2:1).
