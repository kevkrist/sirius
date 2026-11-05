# X2: cross-GPU exchange / shuffle at 4 GPUs (TPC-H SF1000, host-pinned, bb557b1a) — v2

Scope: cross-cutting. Covers what moves between GPUs, what it costs, and what each remedy is worth, priced as **wall time
on the 4-GPU suite total**. Gap to linear: 20.04 - 8.96 = **11.08 s**.

v2 adds the config-flip runs that finished after v1: `batch4g`, `best`, `hpb256`, `hpb4g`, `x_g1_batch4g`, `x_g1_best`.
It also adds an nsys sync-wait split and closes the stream-pool question.

Artifacts:
- `X2/` (v1): `x2_nsys.py`, `g4_summary.csv`, `perquery_table.md`, `pricing_*.txt`.
- `X2/v2/`:
  - `x2_class.py`, output `class_{baseline,batch4g,best,hpb4g,hpb256,joinpol}.txt`.
  - `x2_gprep.py`, output `gprep.csv`.
  - `x2_syncwait.py`, output `syncwait.txt`.
- Flattened Quent: `X2/qf/<run>`.

## TL;DR

1. **Size of the problem.**
   - The exchange family takes **5.58 s of baseline g4 wall**. It is the hash PARTITION tasks plus the consumer's
     cross-GPU clone in Preparing.
   - The same work at g1, divided by 4, is **0.43 s**.
   - The excess of **5.15 s is 46% of the gap**.
2. **The biggest exchange lever is per-fragment cost, not bytes.**
   - `scan_task_batch_size` 4GB gives 4x fewer producer tasks, so 4x fewer fragments. The bytes stay the same (294 GB
     exchanged).
   - Exchange excess falls from **5.15 to 3.05 s (−2.09 s)**.
   - PARTITION thread-time falls from 30.0 to 7.3 s. Clone thread-time falls from 25.1 to 20.7 s.
   - That accounts for 71% of batch4g's −3.13 s g4 suite gain (20.04 → 16.91).
   - The same-config gap falls from 11.08 to 8.65 s. The exchange part of that closure is 2.09 of 2.43 s.
3. **The hash-work slowdown inside exchanges is a per-task artifact.**
   - At baseline, natural-class PARTITION wall share is 965 ms at g4 vs 404 ms at g1/4.
   - At batch4g it is **372 vs 309 ms**, so 0.57 s of slowdown becomes 0.06 s.
   - This supports X1 (per-call allocator/driver contention), not GPU-side slowness.
4. **Broadcast policy (joinpol) mostly overlaps with batch4g.**
   - At baseline it is worth −0.97 s blanket and −1.40 s oracle.
   - On top of batch4g it is worth **0.00 s blanket** (16.91 → 16.92) and **−0.50 s oracle**: Q3 −0.36, Q9 −0.05,
     Q12 −0.04, Q5 −0.03. Q14 +0.22 and Q7 +0.16 regress.
   - At g1 it also flips Q3 STANDARD → BUILD_PROBE (−0.89 s), so the same-policy gap *grows* (best: 8.94 s vs 8.65 s).
5. **Clones are still the largest residual.**
   - At batch4g, clone wall share is **2.50 s** and is byte-bound: 66.6% of the moved bytes cross the switch through host
     staging, at 3.6x the per-byte cost of a peer copy.
   - Of the sync wait, 62-97% is the clone's **own** DMA (Q3: 4,978 of 5,123 ms; Q18 is the low end at 62%). Excess
     driver latency is 11-19 µs per sync.
   - **No foreign kernels ran on any clone stream** (Q3/9/18/21). The shared 16-stream pool is not a factor (closes
     CRITIC N4).
   - The cost is per-buffer serialization (D2H → sync → H2D → sync) and copy-engine queueing.
   - Pipelined or batched clones are worth **≈1.4 s** after batch4g (2.2 s at baseline).
6. **Smaller items.**
   - Switch-aware placement is worth **0 s** for hash shuffles: it is a uniform matrix, and 2/3 of moved bytes are
     cross-switch under any bijection.
   - The exchange does **not** overlap scan H2D (37 ms overlap in total), so the microbenchmark's 11 GB/s-per-pair
     contention prices nothing today.
   - Bloom HOST staging costs ≤0.05 s (unchanged by batch4g; halved by broadcast).

**Exchange remedies priced on the baseline gap (11.08 s), overlap-aware.** The exchange family can recover about
**4.5 of its 5.15 s**:

| Order | Remedy | Wall s | How priced |
|---|---|---|---|
| 1 | Bigger producer batches (config, measured) | 2.09 | Measured |
| 2 | Cost-aware broadcast (config, measured oracle) | 0.50 | Measured oracle |
| 3 | Pipelined clones (code) | 1.4 | Estimated |
| 4 | Remove residual N-forced partitioning (code/policy) | ≈0.4-0.6 | Estimated |
| — | Remaining: cross-switch link floor through host staging plus forced-partition remnants (hardware/topology) | about 0.6 | — |

## Ranked, priced findings (wall time on the g4 suite)

| # | Finding | Kind | Wall s (baseline gap 11.08) | Wall s left after batch4g | Confidence | Pricing |
|---|---|---|---|---|---|---|
| X2-A | Per-fragment fixed cost in exchanges. 627 producers × P fragments: each fragment is hash-partitioned, cloned (≈0.15-0.2 ms fixed per clone) and concatenated separately. | design + config | **2.09** (measured: exchange excess 5.15 → 3.05 s at batch4g) | — | high | Quent class decomposition, g1/g4 pairs at the same config |
| X2-B | Synchronous per-buffer cross-GPU clones (D2H → sync → H2D → sync per leaf buffer, 1 MiB chunks, 6.4 syncs per staged clone, no overlap of legs or clones) | mechanical | 2.2 standalone | **1.4** | medium | Clone share × (1 − achievable/union). Achievable = per-query link floor (45 GB/s per switch downstream, 50 GB/s per peer pair) × byte ratio, plus 25 µs per clone. |
| X2-C | Shuffle joins where broadcast or local would do. BUILD_PROBE P=min(nat,N), `max_broadcast_join_size` 256 MiB, `max_build_hash_table_bytes` 2 GB. | config/design | 1.40 oracle / 0.97 blanket | **0.50 oracle** / 0.00 blanket | high | `x_g4_joinpol`, `x_g4_best` vs `x_g4_batch4g` per query |
| X2-D | N-forced partitioning: P≥N floor on exchanges that pass through at P=1 | design | 2.40 exposure | 1.17 exposure; **0.76** after joinpol; realistic recoverable ≈0.4-0.6 | high | Forced-class excess, Quent |
| X2-E | Cross-switch topology: 66.6% of moved bytes are staged through host. Hash shuffles are uniform. | hardware | Placement **0**. Key co-location ≤1.8. | Co-location ≤1.4 (overlaps X2-B) | medium | Staged clone share × (1 − peer/staged per-byte cost ratio 0.32) |
| X2-F | Slower hashing of identical exchange work at g4 (= X1/M2) | mechanical | 0.57 | **0.06** | high | Natural-class PARTITION share g4 vs g1/4 |
| X2-G | Serial fan-in of hundreds of partials (MERGE_GROUP_BY Q1, MERGE_AGGREGATE Q6) | design | 0.17 | **0.05** | high | Fan-in class excess (151 → 47 ms) |
| X2-H | Late accumulated-Bloom publication (always HOST-staged at g4) | design | ≤0.05 (≤0.30 bound) | unchanged | medium | Extra probe bytes × per-GB share |
| X2-I | "Staging contends with scan H2D on uplinks" | measurement correction | **0** | 0 | high | Clone-H2D/scan-H2D overlap = 37 ms over the suite |

Not additive. X2-A, X2-C, X2-D, X2-F and X2-G all shrink with fewer fragments. The "after batch4g" column is the
incremental value at the new operating point.

## 1. Method

**Data.**
- nsys warm iteration 2: `01-nsys/nsys2_g{1,4}`. Events are restricted to each query's map window.
  - The sum of profiled g4 windows is 21.78 s against 20.04 s un-profiled, a ratio of 1.09x. The 1.25x factor in the
    brief was not observed.
- Quent (un-profiled) for g1_a, g4_a, joinpol, batch4g, best, hpb4g and hpb256, iterations 2-4 averaged.

**Wall share.** For an interval on an executor thread, the wall share is ∫ 1/n_active(t) dt, where n_active counts
tasks in preparing..finalize across all 16 threads.
- Wall shares of all tasks sum to the busy wall.
- This converts thread-time to wall time, charging serial phases fully and fully parallel phases at 1/16.
- **Validated in v1.** The joinpol per-query deltas were predicted within about 5%: Q3 408 vs 401 ms, Q5 241/237,
  Q8 265/252, Q9 266/283.

**Exchange classes** (`x2_class.py`). PARTITION pipeline k is paired with its consumer k+1, by the same pipeline id at
both counts.
- **forced**: g1 partition thread-time is under 10% of g4. The exchange is pass-through at P=1
  (`src/op/sirius_physical_partition.cpp:158`, `count < 2`).
- **fan-in**: the consumer has one task.
- **natural**: everything else.
- Exchange excess = (g4 PARTITION share + g4 clone share) − (the same at g1)/4.

**nsys classification.**
- Clones are NVTX `convert:gpu_to_gpu:{peer_copy,host_staging}`
  (`cucascade/src/cudf/representation_converter_builtins.cpp:892-960`).
- Memcpys are tied to clones through correlationId. Legs are classified by srcKind/dstKind, because CUPTI labels
  foreign-context pinned↔device copies as PtoP.

## 2. What moves (g4 baseline, warm, nsys)

| | Staged (cross-switch) | Peer (intra-switch) |
|---|---|---|
| Clones | 33,667 | 16,830 |
| Bytes | 117.6 GB | 59.0 GB |
| Thread ms | 21,697 | 3,554 |
| ms per clone | 0.64 | 0.21 |
| Syncs per clone | 6.4 | 1.0 |
| Wall share, profiled | 2,910 ms | 502 ms |

- **Cross fraction**: 117.6 / 176.6 = **66.6%**. That is exactly uniform: 8 of 12 directed pairs, at 14.0-15.3 GB per
  directed pair (`X2/g4_pairs.csv`).
- **Volume relative to the scan**: the exchange adds 9.4% to the 1,248 GB scan H2D.
- **By consumer**:

  | Consumer | Staged | Peer |
  |---|---|---|
  | CONCAT | 15.1 s / 82.6 GB | 2.1 s / 41.5 GB |
  | MERGE_GROUP_BY | 5.8 s / 25.8 GB | 1.3 s / 12.9 GB |
  | DENSE_COUNT_JOIN (Q13) | 0.72 s / 9.2 GB | |

- **Same-size clones**: CONCAT 0.79 vs 0.22 ms, staged vs peer (3.6x).
- **Per-leg DMA rates** are healthy: D2H 37 GB/s, H2D 32 GB/s, peer 38 GB/s.

**Per query** (staged clones / GB, peer clones / GB, ms per clone staged/peer, syncs per staged clone, exchange excess
in ms):

| Q | staged | peer | ms/clone st/pe | sync/st | excess base | excess batch4g | excess best |
|---|---|---|---|---|---|---|---|
| 1 | 314/0.0 | 156/0.0 | 0.49/0.19 | 27.0 | 137 | 34 | 33 |
| 3 | 1440/27.2 | 720/13.6 | 3.90/0.80 | 7.0 | 476 | 440 | 113 |
| 4 | 1924/1.4 | 962/0.7 | 0.14/0.07 | 4.0 | 235 | 88 | 119 |
| 5 | 1606/3.2 | 803/1.6 | 0.34/0.11 | 8.2 | 324 | 144 | 86 |
| 7 | 360/3.3 | 180/1.6 | 0.80/0.24 | 5.7 | 112 | 88 | 208 |
| 8 | 1662/2.0 | 831/1.0 | 0.37/0.13 | 9.9 | 379 | 150 | 113 |
| 9 | 2637/18.7 | 1317/9.3 | 1.50/0.46 | 9.2 | 561 | 385 | 307 |
| 10 | 1692/9.5 | 846/4.7 | 0.89/0.27 | 8.2 | 461 | 306 | 305 |
| 12 | 1582/0.6 | 791/0.3 | 0.25/0.12 | 9.0 | 291 | 86 | 36 |
| 13 | 390/9.2 | 195/4.6 | 1.85/0.51 | 4.7 | 203 | 205 | 198 |
| 14 | 1302/3.3 | 651/1.7 | 0.40/0.13 | 7.1 | 214 | 109 | 309 |
| 15 | 1580/2.2 | 791/1.1 | 0.26/0.12 | 5.0 | 162 | 66 | 66 |
| 18 | 11438/20.0 | 5719/10.0 | 0.40/0.19 | 5.0 | 702 | 357 | 365 |
| 21 | 4960/12.9 | 2478/6.4 | 0.33/0.11 | 4.8 | 694 | 394 | 414 |
| others (2, 6, 11, 16, 17, 19, 20, 22) | | | | | 194 | 201 | 224 |
| **sum** | 33667/117.6 | 16830/59.0 | 0.64/0.21 | 6.4 | **5146** | **3053** | **2890** |

## 3. Clone mechanics: where a staged clone's time goes

- **Code.** `peer_copy_into` (`representation_converter_builtins.cpp:640-694`) works per leaf buffer:
  1. D2H, 1 MiB chunks, on the destination's pooled stream under a source-device guard.
  2. `target_stream.sync()`.
  3. H2D chunks.
  4. `target_stream.sync()`.
- **Extra syncs.** Null masks add one (`:725-740`). Strings add an offsets sync, a 4-byte readback and another sync
  (`:786-815`). There are also a final sync (`:947`) and an initial caller-stream sync (`:902`). Q1 partials reach 27
  syncs per staged clone.
- **New sync-wait split** (`X2/v2/syncwait.txt`):

  | Q | kind | clones | clone thread ms | sync ms | waiting on own DMA | excess (latency) | foreign kernels on clone stream |
  |---|---|---|---|---|---|---|---|
  | 3 | staged | 1440 | 5613 | 5123 | 4978 | 145 (14 µs/sync) | 0 |
  | 9 | staged | 2637 | 3946 | 3073 | 2724 | 349 (14 µs/sync) | 0 |
  | 18 | staged | 11438 | 4601 | 2267 | 1412 | 855 (15 µs/sync) | 0 |
  | 21 | staged | 4960 | 1627 | 1086 | 816 | 271 (11 µs/sync) | 0 |
  | 3 | peer | 720 | 575 | 513 | 499 | 14 | 0 |

- **What the split shows.** Syncs mostly wait for the clone's own copies.
  - Q3: 3.5 ms of own-DMA wait per clone, against about 1.1 ms of pure DMA for 18.9 MB.
  - The copies queue on copy engines behind other threads' clones while the phase is link-bound.
- **Shared streams are not a factor.** `memory_space::acquire_stream`, the 16-stream pool (`memory_space.cpp:117-119`),
  never had another task's kernels on a clone stream. CRITIC N4 is closed.
- **Implication for the remedy.** The lever is pipelining: overlap the D2H of buffer i+1 with the H2D of buffer i,
  issue one sync per clone, and run batched multi-fragment transfers. Shaving sync latency alone is worth only the
  excess column, about 1.6 s of thread-time in these four queries, or about 0.1 s of wall.

## 4. The exchange is a separate, kernel-idle phase (it does not overlap scan H2D)

- In 50 ms buckets, Q3 scans at 108 GB/s, the 2-uplink ceiling, for 0-500 ms.
- It then exchanges for 600-1100 ms at about 60 GB/s staged (30 GB/s per switch) plus 30 GB/s P2P, with kernels
  nearly idle. Q9 and Q21 alternate the same way.
- Clone H2D overlapping same-switch scan H2D: **37 ms in total**.
- The clone-phase union is 4.25 s profiled, 19.5% of g4 wall.
- So the p2p microbenchmark regime (cross pairs at 11 GB/s while H2D runs) does not occur.
- If exchanges were made to stream concurrently with the scan, cross-switch bytes *would* then meet that regime, so
  overlapping is a gain only for intra-switch and latency-bound parts.

## 5. Exchange family decomposition and the per-fragment finding (X2-A, X2-D, X2-F, X2-G)

Excess and shares are wall ms. Thread-time is in parentheses.

| Run (g1 pair) | Exchanges | PARTITION tasks | Forced excess | Natural excess | Fan-in excess | Total excess | g4 PARTITION thread | g4 clone thread | Suite g4 |
|---|---|---|---|---|---|---|---|---|---|
| baseline (g1_a) | 171 | 15,668 | 2,402 | 2,593 | 151 | **5,146** | 30.0 s | 25.1 s | 20.04 |
| joinpol (g1_a) | 171 | 15,629 | 1,063 | 2,756 | 158 | 3,978 | 17.3 s | 17.3 s | 19.07 |
| hpb4g (g1_a) | 171 | 15,555 | 2,509 | 2,373 | 237 | 5,119 | 27.3 s | 18.1 s | 19.91 |
| hpb256 (g1_a) | 171 | 15,857 | 2,443 | 4,699 | 165 | 7,307 | 43.7 s | 47.8 s | 23.66 |
| **batch4g** (x_g1_batch4g) | 171 | **3,972** | 1,167 | 1,839 | 47 | **3,053** | **7.3 s** | 20.7 s | 16.91 |
| **best** (x_g1_best) | 171 | 3,946 | 757 | 2,088 | 45 | **2,890** | 4.5 s | 13.9 s | 16.92 |

**Reading the table.**
- **Fragment count dominates the forced and natural PARTITION cost.**
  - batch4g cuts PARTITION tasks 3.9x for the same 294 GB, and PARTITION thread-time 4.1x (30.0 → 7.3 s).
  - Forced PARTITION share drops from 1,335 to 519 ms.
  - Q8's lineitem probe (0.06 GB after Bloom, 627 tasks at 3.2 ms each) is the archetype: 241 ms of its 1.17 s wall
    at baseline.
- **hpb256 is the converse.** P goes up about 4x, and clone thread-time rises from 25.1 to 47.8 s for the same bytes.
  - That is +2.16 s of exchange wall share, against +3.62 s of suite time.
  - Implied fixed cost is about 0.15-0.2 ms per extra clone.
- **hpb4g** leaves exchange excess unchanged (5,119 ms). Q18 −0.31 s comes from fewer MERGE_GROUP_BY partial
  fragments, offset elsewhere (Q9 +0.11).
- **Natural-class hash work scales once tasks are big.**
  - g4 PARTITION share vs g1/4: 965 vs 404 ms at baseline; **372 vs 309 ms** at batch4g.
  - So X2-F (0.57 s) is a per-call overhead, consistent with X1's allocator contention (≈1.2M mallocs plus frees per
    suite, 56 µs mean at 16 threads). Do not double-count it with X1.
- **Clone thread-time barely moves with batch4g** (25.1 → 20.7 s). The residual is byte-bound (X2-B, X2-E).
- **best vs batch4g.** Clone thread-time falls to 13.9 s, but clone wall share rises to 2.59 s. Broadcast builds are
  replicated in low-concurrency phases (Q7 79 → 199 ms, Q14 100 → 309 ms).
- **Residual forced excess at best: 757 ms.** It is Q21 313, Q10 220, Q4 66, Q18 59, Q22 23 and Q17 19.
  - These are the joins that joinpol does not flip: Q10 orders→lineitem, Q21 joins 22/25 and 37/40, Q4 join 12. The
    likely causes are the probe/build ratio estimate or the right/semi-family gates (`sirius_physical_hash_join.cpp:862-867`).
    Unverified.
  - The rest are small group-by exchanges forced to P=4: `partition_sizing.hpp:86-120`, floor N once input ≥16 MB×N.
  - BUILD_PROBE `min(natural, N)` is at `sirius_physical_hash_join.cpp:873`.

## 6. Remedies, priced

### (A) Fewer, larger fragments. Measured: −2.09 s of exchange excess

- **Config route.** `scan_task_batch_size` 4GB, measured. 8GB is worse: 17.34 s.
- **Code route, same effect without coupling to scan granularity.** Coalesce per-GPU producer output before hashing:
  hash one batch per GPU per exchange, not 627. Then ship one packed transfer per (source, destination) pair, not per
  fragment.
- **Gap closure.**
  - batch4g g4 16.91 s vs g1 33.04 s, ideal 8.26 s, so the gap is **8.65 s**.
  - Of the 2.43 s gap closure, 2.09 s is the exchange.
  - The remaining 0.34 s is in scan and other per-task costs (X1/X3).

### (B) Pipelined / batched clones. 2.2 s standalone; **≈1.4 s after batch4g** (1.39 at best)

- **Pricing formula.** Per query: saving = clone share × (1 − achievable / union).
  - Union ≈ share / 0.855, the baseline Quent/nsys ratio.
  - Achievable = (link floor × byte ratio + 25 µs × clones / 16 × count ratio) × 0.92.
  - Link floor = max(per-switch staged bytes / 45 GB/s, per-pair peer bytes / 50 GB/s), from `X2/pricing_d.txt`.
- **Biggest wins after batch4g**: Q3 196, Q9 178, Q21 176, Q18 151, Q13 123, Q10 121, Q14 69, Q5 62, Q8 58 ms.
- **Remedy shape** (`peer_copy_into`):
  - Issue all D2H chunks for all buffers of a clone, record an event, then make the H2D on the destination stream wait
    on it (`cudaStreamWaitEvent`), with one host sync at the end.
  - Or use `cudaMemcpyBatchAsync`.
  - Double-buffer the pinned bounce blocks so the legs overlap.
  - Take string chars sizes from the source column, with no 4-byte readback (`:786-815`).
- **Floor left after this remedy**: about 1.1 s of clone wall share at batch4g (2.50 − 1.42). That is the cross-switch
  bytes at the host-staged rate (hardware/topology, X2-E).

### (C) Cost-aware broadcast / local probe. **0.50 s after batch4g** (oracle); 1.40 at baseline

- **Code.** `compute_hash_join_partition_strategy` (`sirius_physical_hash_join.cpp:824-912`): broadcast needs
  build < 64 MB, or build < `max_broadcast_join_size` (256 MiB) with estimated probe/build ≥ 1.25·N. It also needs
  rows × 16 B < `max_build_hash_table_bytes` (2 GB in our yaml).
- **What a broadcast removes.** The probe uses `partition_mode::local`: no hash and no movement. The accumulated Bloom
  filter is gone too (HOST publications 60 → 28 per 4 iterations).
- **Byte rule.** Broadcast moves (N−1)·B and a shuffle moves (N−1)/N·(B+P), so broadcast wins when P > (N−1)·B. That is
  P > 3B at N=4.
- **Per-fragment latency also counts.** That is why Q7 (4.5 GB build) and Q14 (6.5 GB) regress under the blanket 8 GB
  flag. They regress more at batch4g (+0.16, +0.22 s), because the shuffle they replace got cheaper.
- **Same-policy caveat.** At g1 the flag flips Q3 STANDARD → BUILD_PROBE (−0.89 s at g1 batch4g), so the scaling
  *ratio* falls even though g4 wall improves. Price the honest gap with equal policy.

### (D) Residual N-forced partitioning. ≈0.4-0.6 s realistic, 0.76 s exposure after best

- Remedies:
  - Broadcast builds whose natural partition count is 1 when probe ≫ build. That is the gate fix for Q10/Q21/Q4.
  - Do not floor P at N for small group-by inputs, where the consumer total is under about 1 GB.
- Exposure is the whole forced excess. A broadcast still replicates the build (Q3: 29 → 97 ms), so recovery is about
  half to two-thirds of it.

### (E) Switch-aware placement / key co-location. Placement 0 s; co-location ≤1.4 s after batch4g (overlaps B)

- **Placement.** `gpu_ids[p % N]` (`src/op/partition_placement.hpp:58-70`).
  - Producers are uniform: pin is round-robin and scan tasks are round-robin.
  - So any bijection of partitions to GPUs leaves 1/2 of all bytes (2/3 of moved bytes) cross-switch. The measured
    pair matrix confirms this.
  - Island-first two-level exchange does not cut cross bytes either.
- **Co-location.** Range-place lineitem and orders pinned chunks by orderkey per switch. The orderkey exchanges then
  become intra-switch or local:
  - the Q3/4/5/10/12/18/21 joins;
  - Q18 GROUP BY l_orderkey (20 GB staged).
- **Bound.** Staged clone share at batch4g is about 2.1 s. × (1 − 0.32 peer/staged per-byte ratio) ≈ **1.4 s**, before
  overlap with (B). Beyond (B), roughly the 1.1 s floor × 2/3 for the orderkey subset ≈ 0.5 s.

### (F) Bloom HOST staging. ≤0.05 s, unchanged

- Publication is all-or-nothing host-staged at g4 (`sirius_dynamic_bloom_filter.cu:1080`, see `bloom_peer_access.md`).
- There are 15 publications per hot iteration. Summed latency is 301 ms per iteration at baseline and 299 at batch4g.
  Broadcast halves it (28 per 4 iterations).
- Probes never wait, so the cost is only extra unfiltered probe bytes. That is +9.1 GB over 10 exchanges at baseline,
  priced at about 8 ms of exchange share plus about 15 ms/GB of join probe. Total ≤0.05 s, upper bound 0.30 s if
  latency were on the critical path.
- The queries with late accumulated publication (Q5/Q8/Q10/Q21) show exchange inputs within 2% of g1.

## 7. Not priced / open

- **Q13 DENSE_COUNT_JOIN** (custkey, 13.8 GB moved, 0.2 s excess at every config). Only (B) and (E) apply. The excess is
  insensitive to batch and broadcast.
- **Why joinpol does not flip Q10/Q21/Q4.** No debug logs. The gates at `sirius_physical_hash_join.cpp:862-889` have to
  be read against the cardinality estimates.
- **The 1.1 s cross-switch floor assumes host-staged rates.** If staging ran at the uplink limit (52 GB/s down per
  switch, full duplex), the floor would be about 0.9 s. This is a hardware limit of this PCIe topology. There is no P2P
  across switches.
- **No nsys at batch4g.** The (B) pricing at batch4g scales baseline per-query floors by Quent GPU-origin input bytes,
  and latency by consumer task counts.
