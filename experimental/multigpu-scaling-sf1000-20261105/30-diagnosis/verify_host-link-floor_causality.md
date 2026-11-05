# Verify rank 1 "host-link-floor": causality check

The claim is 3.48 s (31% of the 11.08 s gap): two Gen5 uplinks serve four GPUs.
Verdict: **the mechanism holds, but the price is overstated.** The corrected hard-floor share is **2.89 s (26%)**.
The link's total causal footprint (this row plus rank 5) is larger, about 3.8 s if the hardware were fixed. The split
between the two rows is a convention.

Scripts are in `30-diagnosis/verify_host-link-floor/`. `switch_rate.py` is an independent per-switch rate check on
nsys. No GPU work was run.

## 1. The mechanism is causal (confirmed)

**Code.** `src/compression/compressed_representation.cpp:62-86` issues one `cudaMemcpyAsync` H2D per 1 MiB host block,
as cited.

**g2s vs g2x isolates the link.** Both runs use the same software at N=2; only uplink sharing differs.
- Link-bound queries with a dedicated uplink per GPU (g2x) scale **1.87-1.94x**:
  - Q17: 0.948 s, against T1/2 = 0.919
  - Q19: 1.113 s, against T1/2 = 1.048
  - Q20: 0.862 s, against T1/2 = 0.808
- With a shared uplink (g2s) the same queries reach only 1.13-1.17x.
- g2x also pays cross-switch host staging, so the 7.19 s g2s → g2x gain is a conservative measure of the link effect.

**Line-rate saturation at g4** (independent recomputation, `switch_rate.py`, nsys2_g4 iter2, windowed):

| Query | Per switch | Busy (union) | Aggregate |
|---|---|---|---|
| Q17 | 54.8 / 54.8 GB/s | 94-95% | 108.9 GB/s |
| Q20 | 54.6 / 54.6 GB/s | 91% | 107.4 GB/s |
| Q19 | 53.3 / 53.4 GB/s | 91% | 103.5 GB/s |
| Q2 (raw tables) | 55.5 / 55.5 GB/s | 73-75% | 104.5 GB/s |

- The peak 20 ms bins reach 109-113 GB/s aggregate.
- Per-GPU rate while busy is only about 30 GB/s, so the bottleneck is the switch uplink, not the GPU link.
- Microbench: {0,2} gives 102 GB/s and {0,1,2,3} gives 105 GB/s. The uplink is the limit, not the host or root
  complex.

**Alternatives rejected:**
- Allocator or prepare overhead does not bind the link-bound queries. batch4g and thr8 leave Q17/Q19/Q20 unchanged
  (0.824/0.937/0.767), and gpupin drops them to 0.20/0.47/0.22.
- The g4 per-switch rates match the g1 raw-path rate (55.3-55.5 GB/s), so the 1 MiB copy pattern does not cap the rate.

## 2. The price is overstated: the floor uses 52 GB/s, but the engine sustains 55

- `X3/floors.py` uses `SW=52.0`, the 1 GiB single-stream microbench rate. The engine itself sustains 54.5-55.5 GB/s
  per switch:
  - Q2, Q11, Q16, Q17, Q20, per `g4_link.csv`;
  - g1 raw-path queries, 55.3-55.5 GB/s.
- A hard floor has to use the best demonstrated rate. 52 GB/s inflates every link floor by 6%.

| Floor rate (per switch) | Floor share, base | Floor share, batch4g | Floor share, best | g4 suite floor, base |
|---|---|---|---|---|
| 52 GB/s (claim) | 3.48 s (31%) | 4.01 s (46%) | 4.23 s (47%) | 12.43 s |
| 54 GB/s | 3.08 s | | | |
| **55 GB/s (measured)** | **2.89 s (26%)** | **3.42 s (40%)** | **3.61 s (40%)** | **11.84 s (3.0x)** |

- With the 55 GB/s floor capped per query by the gpupin no-link saving (clean queries only), the share is **2.82 s**.
- At 52 GB/s the floor share exceeds even the no-link saving that gpupin measured:
  - Q5: floor share 0.200 s, gpupin saving 0.125 s
  - Q7: floor share 0.200 s, gpupin saving 0.153 s
  - Q9: floor share 0.190 s, gpupin saving 0.141 s
  - Q15: floor share 0.160 s, gpupin saving 0.130 s
  - Removing the link entirely cannot save less than the hardware share alone. These four queries therefore confirm
    that 52 GB/s overstates the floor.
- Corrected per-query floor shares at 55 GB/s (s):

  | Q19 | Q17 | Q8 | Q20 | Q12 | Q7 | Q5 | Q21 | Q14 | Q9 | Q2 | Q15 | Q10 | Q6 | Q11 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | 0.34 | 0.32 | 0.29 | 0.28 | 0.20 | 0.17 | 0.17 | 0.15 | 0.15 | 0.14 | 0.13 | 0.13 | 0.12 | 0.10 | 0.09 |

## 3. Causality caveat: the floor is not the lever

The hard floor is a max-model: it assumes link time perfectly overlaps everything else. g2s vs g2x lets me test that
directly at 55 GB/s.

| N=2 model | Predicted g2s − g2x | Measured |
|---|---|---|
| Max-model: T2s = max(T2x, bytes/55) | 3.33 s | **7.19 s** |
| Fully additive model | 11.4 s | |

- The measured value implies a sensitivity of 0.63.
- Queries with a max-model floor of 0 still gain 0.35-0.6 s from doubling the uplink: Q9 0.61, Q21 0.56, Q10 0.35.
- That is the inter-pipeline serialization in rank 5: link-bound scan phases run strictly between link-idle exchange
  phases.
- So a hardware fix (one uplink per GPU) would save more than the floor share:
  - 0.63 × (11.97 s per-switch busy at g4 / 2) ≈ **3.8 s ΔT4**, which equals Δgap here because g1 already has its own
    uplink.
- gpupin (−3.82 s on 19 queries) is **not** a clean test of this hardware counterfactual:
  - It removes all link time, including the share that also exists at g1. Q17 drops to 0.20 s, below T1/4 = 0.46.
  - It changes memory pressure: Q10, Q16, Q21 and Q22 regress, and Q1, Q3 and Q18 log downgrade errors.
  - It brackets the floor share only by coincidence.
- The split between rank 1 and rank 5 is a convention. The correction above moves about 0.6 s out of rank 1. Part of
  it belongs in rank 5 (link phases a pipelined engine would hide).
  - Rank 5 itself was priced as a per-phase link price at 105 GB/s minus the floor. Both terms should be recomputed at
    110 GB/s, and the combined link footprint (rank 1 + rank 5, 4.68 s) probably falls to about 4.1-4.2 s.
- One unverified assumption: whether the host or root complex could feed 4 × 55 = 220 GB/s with 4 uplinks. Bidir
  aggregate of 175 GB/s shows it can feed at least 175.

## 4. The rest of the claim checks out

- Scan bytes: 1254 GB at g1, 1249 GB at g4.
- Per-switch link-busy: 25.5 s at g1, 11.97 s at g4.
- batch4g leaves the link-bound queries unchanged.
- g2s 29.40 s, g2x 22.21 s.

## Corrected numbers

- Rank 1 hard-floor share: **2.89 s (26% of the gap)**, range 2.82-3.08.
- It is still rank 1, ahead of the allocator at 1.80 s.
- Best possible g4 suite at today's bytes: about **11.8 s (3.0x)**, not 12.4 s (2.9x).
- After batch4g the link is 3.42 of the 8.65 s gap (40%), not 46%.
- Lever of a hardware fix alone: about 3.8 s ΔT4, which includes part of rank 5.
