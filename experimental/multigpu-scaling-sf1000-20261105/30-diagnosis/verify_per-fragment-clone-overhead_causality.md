# Verify (causality): #3 per-fragment cross-GPU clone overhead — claimed 1.72 s

Verdict: **OVERSTATED**. The mechanism is real and on the critical path. But about 0.5 s of the 1.72 s is byte-bound
staging serialization (#4's mechanism) or gathers that do not respond to fragment count. Two of the cited dose-response
numbers also credit PARTITION and other effects to clones.

**Corrected price: ≈1.2 s (range 1.05–1.35), 11% of gap.** The ≈0.5 s removed moves to #4 (bytes and staging
serialization) and a little to #2 (allocator calls inside clones).

## Checks run (CPU only, no GPU)

### 1. Code path re-read: confirmed

- Placement: `partition_placement.hpp:59-70` round-robins partition p to `gpu_ids[p%N]`.
- Prepare loops over batches serially: `sirius_physical_operator.cpp:139-190`.
- `batch_lock_utils.hpp:123-138` calls `clone_to` per remote batch.
- `convert_gpu_to_gpu` (`representation_converter_builtins.cpp:902` and `:947`) syncs at entry and exit.
- `peer_copy_into` (`:640-694`) does D2H → sync → H2D → sync per leaf buffer.
- **New:** `find_host_pool(resolve_gpu_numa_node(dst))` at `:612` runs `cudaDeviceGetPCIBusId` plus an `ifstream` read of
  sysfs `numa_node` (`:549-567`) on **every staged leaf buffer**.
  - nsys `fopen` inside staged clone ranges: Q12 18 ms, Q8 28 ms, Q18 97 thread-ms.
  - It is a small mechanical cost. It belongs to this bucket.

### 2. "1 consumer task per partition, 3 of 4 threads idle": confirmed

Quent `g4_a` iter3: every clone-heavy consumer pipeline has exactly 1 task per GPU (Q12 P2/P5, Q8 P29/P32/P35,
Q21 P14/P29/P41). Q1 MERGE has 1 task in total. The serial clone loop therefore *is* the phase's critical path, so
thread-time per task ≈ wall.

### 3. Is the per-clone fixed cost intrinsic, or caused by another bottleneck (allocator lock, contention)?

nsys g4 iter2, inside `convert:gpu_to_gpu:*` ranges (thread ms):

| Q | kind | n | wall | sync | memcpyAsync issue | cudaMallocFromPoolAsync | fopen | DMA |
|---|---|---|---|---|---|---|---|---|
| 12 | staged | 1582 | 403 | 190 | 85 | 18 | 18 | 32 |
| 12 | peer | 791 | 94 | 26 | 22+10 | 9 | – | 7 |
| 8 | staged | 1662 | 617 | 312 | 119 | 28 | 28 | 95 |
| 1 | staged | 314 | 154 | 61 | 32 | 9 | 9 | 4 |
| 18 | staged | 11438 | 4601 | 2268 | 869 | 559 | 97 | 1011 |

- Allocator share inside clones is 5–7% on the count-bound queries (Q1, Q8, Q12) and 15% on Q18.
- Per-clone cost does not inflate from 2 to 4 GPUs.
  - Peer: g2s ≈0.118 ms (fam_semijoin Q12 CONCAT(3) 36.8 ms / ~313 clones) vs g4 0.115 ms.
  - Staged: g2x ≈0.21 vs g4 0.25 ms.
- So the fixed cost is intrinsic: per-buffer sync latency plus API issue. It is not an artifact of the #2 lock. The
  stated mechanism is the cause.
- The fixed cost roughly **doubles when the clone is host-staged** (0.21 vs 0.12 ms). The count term and the staging
  (#4) mechanism are therefore entangled.

### 4. Dose-response, isolated to clones

Using Quent GPU-origin prepare wall share (∫1/n_active), from `10-analysis/X2/v2/gprep.csv` plus my run of
`x2_gprep.py` on `X2/qf/x_g4_hpb{256,4g}`:

| run | clone wall share | Δ vs base |
|---|---|---|
| baseline | 3.34 s | – |
| batch4g (3.3–4.3x fewer fragments, same bytes) | 2.50 s | **−0.84** |
| hpb256 (≈4x P) | 4.64 s | **+1.30** (Q18 alone +1.00) |
| hpb4g | 3.48 s | +0.13 |

- **Claim evidence "exchange excess 5.15 → 3.05 s" is misattributed.**
  - Only 0.84 s of the −2.09 s is clones.
  - The rest is PARTITION: thread 30.0 → 7.3 s. That is #2 allocator plus #6 forced partitioning.
- **Claim evidence "hpb256 +3.62 s suite" is misattributed.**
  - Only 1.30 s of it is clones.
  - The rest is PARTITION (thread 30 → 43.7 s) and a Q13 DCJ hazard (+0.7 s, fam_groupby).
- **Count-bound part.** I fitted share_q = a_q (bytes) + b_q × fragments per query, solving baseline against batch4g,
  bytes equal.
  - Count-bound clone share at baseline = **1.19 s**.
    - Biggest terms: Q18 202, Q21 160, Q8 140, Q1 135, Q12 115, Q5 97, Q10 79, Q4 67, Q14 61, Q15 59, Q9 42.
    - Q3 and Q13 are 0 (byte-bound).
  - Plus ≈0.05 s for the 6 queries that cannot be fitted because their plan or bytes changed.
  - So ≈1.24 s. The remaining ≈2.1 s of clone wall share is byte-bound, which is #4 territory.

### 5. Family-by-family re-check of the 2.02 s that feeds #3

| family | claimed | corrected | why |
|---|---|---|---|
| scanagg F3 | 256 | 256 | Clean count dose (Q1 148 → 42, Q14 98 → 27) |
| joinship F3 | 184 | ≈120 | Q3's 43 ms is byte-bound: CONCAT(14) clone prepare is unchanged under batch4g (5879 → 5796 thread-ms). Q10 only |
| semijoin F2a | 434 | ≈400 | b × fragments fits. My wall-share fit gives 342 |
| multijoin F2 | 639 | ≈473 | batch4g cuts F2 639 → 276 at 4.3x fewer fragments. The count part is 363 × 4.3/3.3 ≈ 473. The ≈166 non-count part is staging serialization (#4) |
| groupby F1 count | 253 | 253 | Measured dose. About 50–100 ms is allocator inside clones (overlaps #2) |
| small F2 | 161 | ≈10 | **No fragment dose.** Q2/11/16/22 clone share is 130 → 123 ms under batch4g, and the family *loses* 41 ms. These are byte/staging gathers with P<N: #4 or #8 |
| subq F3 | 88 | ≈60 | Q15 share 92 → 50 implies 59 count-bound |
| **sum** | **2025** | **≈1572** | |
| − frees moved to #2 | −300 | −300 | Unchanged |
| **#3** | **1.72 s** | **≈1.27 s** | |
| − mallocs inside clones (overlap with #2) | – | −0.05…−0.1 | |

Two independent routes give the same answer:
- the bottom-up family correction gives 1.27 s;
- the global wall-share fit gives 1.24 s.

**Final: ≈1.2 s (1.05–1.35).**

### 6. Would removing it return that wall time? (critical path and link ceiling)

- **Yes, mostly.**
  - Clone phases are kernel-idle and link-idle: 37 ms of scan-H2D overlap suite-wide (X2 §4).
  - They run 1 task per GPU, so they sit on the critical path.
  - The link floor (#1) does not absorb them.
- **Caveat 1.** If #5 (scan lookahead) were fixed first, part of this would hide under scan link time, so the two
  levers are not additive.
- **Caveat 2.** The code remedy that removes the count term is batching the clone with one sync per (src, dst, task).
  That same remedy also removes most of #4's serialization. Price the two together, not by adding them.

## Corrections to the claim text

- **Evidence lines to fix:**
  - "exchange excess 5.15 → 3.05 s" should read "clone wall share 3.34 → 2.50 s (−0.84)".
  - "hpb256 … suite +3.62 s" should read "clone share +1.30 s (Q18 +1.0)".
  - "Q18 MERGE prepare 27 → 6077 thread-ms" is ≈60% byte-bound. The count part is ≈200 ms of wall share.
- **Affected queries:** drop Q16 and Q22 (no fragment dose) and Q3 (byte-bound). Q9 is mostly byte-bound (count 42 of
  361 ms).
- **Add the sysfs `ifstream` per staged leaf buffer** (`representation_converter_builtins.cpp:549-567,612`). It is a
  trivial fix: cache the NUMA node per device.
