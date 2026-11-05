# Verify #3 per-fragment clone overhead (1.72 s): measurement lens

**Verdict: OVERSTATED.** Corrected share is about **1.2 s** (range 0.9-1.45), not 1.72 s.

- The mechanism and the raw numbers hold.
- The overstatement comes from the count-vs-bytes split. Several families booked byte-proportional staging inefficiency
  as "per-fragment" cost.
- That surplus, about 0.5 s, belongs to #4. #4's 1.23 s is priced as a pure 77 GB/s floor, so it is understated by the
  same amount.
- The FRAG + BYTES total is consistent with the measured clone wall, so this is a reallocation between rows. The
  waterfall total does not change.

All work, scripts and outputs are in `30-diagnosis/verify_frag_meas/`:
- `gprep.py`: clone thread-time.
- `clonewall.py`: clone wall attribution.
- `synccount.py`: nsys API counts per clone range.
- `q_x_g4_batch4g`, `q_x_g1_batch4g`, `q_x_g4_hpb256`: flattened Quent for those runs.

## 1. Re-derived numbers (all confirmed)

| Claim | Re-derived | Source |
|---|---|---|
| Q1 MERGE_GROUP_BY prepare 134-136 ms at g4 vs 3 ms at g1 | 134.4 / 136.0 / 135.3 vs 3.1 / 3.0 / 3.1 (iters 2-4). batch4g: 35.2 / 33.3 / 33.1 | Quent, un-profiled |
| Q18 MERGE prepare 27 → 6077 thread-ms | g1 30.5 / 26.5 / 24.9; g4 5906 / 6077 / 5975. **batch4g 4207 / 4235 / 4145 (only −31% for 4.3x fewer fragments)** | Quent |
| 470 clones in Q1 merge; 6.4 syncs per staged clone | Q1: 470 clones (314 staged). Suite: 50,497 clones, 33,667 staged, **6.40** cuStreamSynchronize per staged clone. Per query: Q1 27, Q12 9, Q5 8.2, Q18 5.0 | nsys2_g4 iter2, per-query windows |
| Clone thread-time 25.1 s; hpb256 47.8 s | g4_a 25.45 s; batch4g 20.81; hpb256 48.2; g1 0.23 | Quent GPU-origin prepare, iters 2-4 mean |
| Code: `partition_placement.hpp:58-70` round-robin; syncs at `representation_converter_builtins.cpp:902, 947, 625/631/665/694` | Present at bb557b1a | source |

### Profiler check

- Suite-wide, nsys clone thread-time is 26.05 s against 25.45 s in Quent: 1.02x.
- For the latency-bound Q1 merge, nsys shows 186.6 ms against 136 ms in Quent: **1.37x**. That is about 32k API calls
  at roughly 1.5 µs of CUPTI cost each.
- The claim prices from Quent (un-profiled), so this does not bias the price. The nsys-derived per-clone latencies
  quoted in the family docs are inflated by up to 1.37x, but they were not used for pricing.

### Allocator overlap inside the clone

Allocator time inside the clone ranges is small:
- Q1: `cudaMallocFromPoolAsync` 13 of 187 ms.
- Q12: 27 of 525 ms.

That is 5-7%. The frees happen at task end, and RANKED already moved 0.30 s of them to #2. No further double count
with #2.

### Iteration stability

Clone wall attribution is stable across iterations:
- g4_a: 3.354 / 3.346 / 3.331 s.
- batch4g: 2.511 / 2.509 / 2.492 s.

No cold or warm confound. Q13 has no clone count part.

## 2. What is wrong: the count vs bytes split

The batch4g dose separates the count-bound part from the byte-bound part: 4.32x fewer fragments for the same bytes.

**Thread-time.** GPU-origin prepare goes from 25.45 to 20.81 s, a drop of 4.64 s.
- Count part = 4.64 × 4.32 / 3.32 = **6.0 thread-s, 24% of clone thread-time**.
- Excluding Q17, which rises from 76 to 503 under batch4g, gives 6.6 thread-s (26%).
- The rest, about 19 thread-s, is byte-proportional.

**Wall.** Clone-attributed wall is ∫ n_gpuprep / n_active dt over all tasks (`clonewall.py`):
- g4_a 3.34 s (pure clone-only time 2.82).
- batch4g 2.50 s (pure 1.94).
- g1 0.10 s, so g1/4 is about 0.025.
- Count part = (3.34 − 2.50) × 1.30 = **1.14 s** (pure-metric version 1.16).
- Thread-proportional alternative: 24-26% × 3.34 = 0.80-0.87 s.

Per query, comparing the count part (top-down, ms) with the FRAG ms the families booked:

| Query | Count part | Family booked | Note |
|---|---|---|---|
| Q1 | 134 | 144 | |
| Q4 | 66 | 91 | |
| Q5 | 89 | 146 | |
| Q6 | 22 | 23 | |
| Q7 | 15 | 65 | |
| Q8 | 125 | 179 | |
| **Q9** | **35** | **249** | |
| **Q10** | **71** | **141** | |
| **Q3** | **0** | **43** | clone wall 429 → 433 under batch4g: purely byte-bound |
| Q12 | 112 | 145 | |
| Q14 | 59 | 77 | |
| Q15 | 54 | 88 | |
| Q18 | 186 | 253 | |
| Q21 | 149 | 198 | |
| Q2 / Q11 / Q16 / Q22 | 11 | 161 | not fragment-sensitive; staged gathers at about 13 GB/s, closer to #4 |

Where the families went wrong:

- **multijoin.** F2 = "CONCAT excess − 77 GB/s floor" (639 ms). This books serialized-staging inefficiency as
  per-fragment cost.
  - Its own batch4g bucketing leaves 276 ms of "overhead" unchanged by 4.3x fewer fragments.
  - The dose-consistent count part is (639 − 276) × 1.30 ≈ 472.
- **joinship.** F3 (184 ms) includes Q3 43, which batch4g shows to be fully byte-bound, and Q10 CONCAT(12) 14 ms,
  which the doc itself calls "bytes-bound". The dose-consistent value is about 80.
- **semijoin** (fitted a + b·n), **groupby** (measured drop) and **scanagg/subq** used the dose correctly.
- **small** (161 ms) has no dose evidence. It is per-exchange staging latency, not 627×P fragments.

Corrected bottom-up total:

| Family | ms |
|---|---|
| scanagg | 256 |
| joinship | about 80 |
| semijoin | 434 |
| multijoin | about 472 |
| groupby | 253 |
| subq | 88 |
| small | 0-161 |
| **Total** | **1.58-1.74 s** |

Minus the 0.30 s of frees moved to #2, that gives **1.28-1.44 s**.

The top-down figure is 0.80-1.16 s. It covers prepare only, so it misses the per-fragment CONCAT op work in the phase
excess.

**Best estimate: 1.2 s (0.9-1.45), about 11% of the gap.**

### Consistency check

Total clone wall at g4 is 3.34 s, against g1/4 of 0.025. RANKED's FRAG + BYTES + moved frees is
1.72 + 1.23 + 0.30 = 3.25 s.

So the rows together match the measured total. The error is only how the total is split between them. #4 should rise
by about 0.5 s, to about 1.7 s. That is consistent with RANKED's own "pipelined clones ≈2.2 s at baseline" lever for #4.

## 3. Evidence items that are misleading as cited

**hpb256 "+3.62 s suite"** is not a clean fragment dose:
- There is no g1 hpb256 run, so it is not a gap measurement. g1 also gets more tasks.
- It includes allocator growth. Q18 PARTITION phase C is 2.8x worse (903 ms), which is #2.
- Clone-attributed wall rises only 3.34 → 4.64 s (+1.29), of a +3.62 s suite change. Q18's clone part rises
  488 → 1486 ms of its +2.11 s.
- The two doses also disagree for Q18:
  - batch4g implies a count part of about 2.4 thread-s.
  - hpb256's ×4 implies about 5.5 thread-s.
  - So per-fragment cost is not constant. It rises with contention.
- Cite +1.29 s of clone wall, and only as a qualitative confirmation.

**"Q18 MERGE prepare 27 → 6077 thread-ms"** is cited as per-fragment evidence, but it is about 59% byte-bound. batch4g
cuts it by only 31% (27 GB moved; this is groupby's BYTES 439 ms). Cite the Q1, Q12, Q8 and Q6 merges instead: those
are fully count-bound, at 75-77% drops.

**"batch4g exchange excess 5.15 → 3.05"** is shared with #2 and #6, as the claim says. It cannot price #3 alone. The
batch4g-attributable clone-wall drop is 0.84 s (measured), which extrapolates to a count part of 1.14 s.

**"3 of 4 threads idle"** holds for the CONCAT and merge phases: 1 task per GPU or per query. It is already reflected
in the wall attribution, which uses ∫ f dt with f = 1 when only clones run. Pure clone-only wall is 2.82 of 3.34 s.

## 4. Corrected wall estimate

- **#3 ≈ 1.2 s** on the g4 suite (range 0.9-1.45, confidence medium-high). The count-bound part is real, with a clean
  dose on Q1, Q4, Q5, Q6, Q8, Q12, Q14, Q15, Q21 and Q18-partial.
- Q3, Q9 and Q13 and the small-table gathers should be removed from its affected-query list, or marked as #4 (bytes).
- Move about 0.5 s to #4 (serialized 2-leg staging, per-byte).
