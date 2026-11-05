# Verify (causality): rank 4 "cross-switch shuffle bytes via serialized 2-leg host staging" (claimed 1.23 s, 11.1%)

Verdict: **OVERSTATED.** The stated mechanism, serialized D2H→sync→H2D legs, is real and can be fixed. It explains
only **≈0.6 s** (range 0.5-0.8) of g4 wall time.

The larger part of the 1.23 s bucket is a different cause: the **byte floor of cross-switch shuffle bytes on the
2 uplinks, ≈1.0 s**. That floor stays even with perfectly pipelined legs, and it would stay with real cross-switch P2P
too. It is topology plus the hash-shuffle design, so only fewer bytes (broadcast or key co-location) removes it.

The remedy claim ("≈2.2 s at baseline, 1.4 s after batch4g") also counts per-clone fixed latency, which belongs to #3.

Scripts and outputs: `30-diagnosis/verify_xswitch/` (CPU-only nsys analysis, plus one microbenchmark of a few seconds,
run under `gpu_lock_run.sh`, in `mb/`).

## 1. Code check: mechanism confirmed

Source: `cucascade/src/cudf/representation_converter_builtins.cpp`.

- `peer_copy_into` starts at `:589`. Pairs without P2P take the host-staged path, which allocates host-pool blocks at
  `:637`.
- Pass 1 (`:640-665`) issues D2H in 1 MiB chunks:
  - on `target_stream`, which is the **destination** memory_space's pooled stream (`:925`);
  - under `src_guard` (`:644`);
  - and ends with `target_stream.sync()` (`:665`).
- Pass 2 (`:668-694`) issues the H2D chunks, then calls `sync()` (`:694`). This happens per leaf buffer, so the two
  legs never overlap inside a clone.
- `wait_for_writer(target_stream)` (`:931`) adds one cross-device `cudaStreamWaitEvent` per clone.

## 2. Data checks (nsys2_g4, iteration 2, events restricted to the map windows)

| Check | Result | Bearing on claim |
|---|---|---|
| Staged volume and wall (`suite_floor_g4.txt`) | 117.6 GB staged, 59.0 GB peer (matches claim). Staged-clone union is 4.12 s profiled. Its overlap with kernels is 0.45 s, and with scan H2D only 0.039 s | Confirms: kernel-idle, no overlap with scan |
| Uplink floor for staged bytes (max per switch of up/57 and down/52 GB/s, per query) | **1.17 s profiled → ≈1.07 s unprofiled**. X3 independently gives 0.97 s (13.40 − 12.43) | This much of the "bytes" cost stays with any pipelining, and with true cross-switch P2P |
| Fixed vs byte cost per staged clone (OLS over 33,667 clones, `clone_regress_g4.txt`) | duration = 0.231 ms + 118 ms/GB. That is **7.8 thread-s fixed (count-bound, #3)** plus **13.9 thread-s byte-proportional**. Peer: 0.104 ms + 30.5 ms/GB | 36% of staged thread-time is per-clone latency, not bytes. The 3.6x "per byte" mixes the two (marginal byte ratio 3.9x, fixed ratio 2.2x) |
| Own DMA execution share of staged clone time (`clone_timeline_*.txt`) | Executed DMA (both legs) is **5.97 of 21.7 thread-s (27.5%)**: Q3 1.10 of 3.90 ms per clone, Q9 0.36 of 1.50, Q18 0.09 of 0.40, Q21 0.12 of 0.33, Q13 1.09 of 1.85 | Contradicts the evidence line "62-97% of the sync wait is the clone's own DMA". That figure counts time *queued behind* the clone's own copies as DMA. The clone is not DMA-bound |
| Engine and uplink utilization vs staged concurrency (`burst_util.txt`) | Even with 12 or more staged clones in flight (Q3, 292 ms), staged D2H/H2D engines are busy only **48%** (max device 53%), and each switch carries **31 GB/s per direction** against 52-57 capacity. Q9: 41%, 28-32 GB/s | There is headroom, so serialization is real, but bounded at about 1.7x in the dense phases |
| D2H chunk dead time (`chunk_gaps.txt`, `gap_fill.txt`) | Back-to-back D2H chunks (next chunk already queued) leave a **median 13 µs, mean 26 µs idle gap**. H2D gaps are median 1.3 µs, mean 3.5 µs. During the D2H gaps the engine is idle 92-96% of the time (Q3: 689 ms of gaps vs 730 ms of D2H busy) | A **second mechanism the claim misses**: the D2H leg is issued on the *destination's* stream for *source* memory (CUPTI copyKind 10, runs on the source device). That runs the D2H at about 50% duty |
| Microbench, single flow, isolated (`mb/results*.txt`) | Foreign-stream 1 MiB D2H: **36.4 GB/s**, own stream: 54.6 GB/s (with K=4 streams, 52.6 vs 53.9). Full clone: current code (serial legs, foreign D2H) 21.1 GB/s; serial legs with own-stream D2H 26.2; **overlapped legs (all D2H+events, then wait_i+H2D_i) 46.5 GB/s** cross-switch, 42.2 same-switch | The remedy works per flow (2.2x). In production about 11 clones are already in flight (time-weighted mean, Q3), so the aggregate gain is capped by the uplink floor |

## 3. Re-pricing (wall seconds, g4 suite, unprofiled ×0.92)

- **Byte-attributable staged wall.**
  - X2 staged wall share is 2.91 s profiled. The byte fraction of staged thread-time is 0.64.
  - So 1.86 s profiled, **≈1.71 s unprofiled**.
- **Floor** (the same bytes moved at uplink rate, legs fully overlapped): **≈1.0-1.07 s.**
  - This is caused by the hash shuffle moving (N−1)/N of the bytes, 2/3 of them across switches, through 2 uplinks.
  - The design and topology cause it, not staging.
- **Removable by the stated mechanism** (pipelined legs, one sync, D2H on a source stream): 1.71 − 1.07 ≈
  **0.64 s**.
  - Per-query cross-check on the byte-heavy queries: Σ (staged union × byte fraction − floor) over
    Q3/9/10/13/18/21/14/5/7 is ≈0.83 s profiled, ≈0.76 unprofiled. That is an upper bound, because it assumes the
    staged union is fully critical (Q3 has 151 ms overlapping kernels).
  - Range: **0.5-0.8 s.** Point estimate: **0.6 s.**
- **The bucket's 1.23 s is mostly floor.**
  - It is built as multijoin "bandwidth floor" 351 ms + groupby "bandwidth part" 439 ms + semijoin F2b 292 ms (≈40%
    floor) + scanagg 70 ms + joinship 77 ms.
  - So about 1.0 s of it is the topology byte floor, and only about 0.2 s is serialization. The other ≈0.4 s of
    serialization excess is booked under #3 (FRAG).
  - The waterfall total does not change. The label and the remedy value do.
- **The remedy value is overstated.**
  - X2-B's 2.2 s (1.4 after batch4g) assumes per-clone overhead falls to 25 µs. That removes most of the 7.8 thread-s
    fixed part, which is #3.
  - For #4's mechanism alone it is ≈0.6 s, both at baseline and after batch4g. batch4g leaves the byte part
    unchanged: clone thread-time goes 25.1 → 20.7 s, with fixed bytes.

## 4. What stands from the claim

- 66.6% of moved bytes are cross-switch. The directed-pair matrix is uniform.
- Switch-aware placement is worth 0 s for hash shuffles.
- Exchange phases are kernel-idle and do not overlap scan H2D (38.7 ms in my count).
- Clone phases reach about 30 GB/s per switch.
- Pipelining the legs is a valid, medium-complexity fix.

**Cheaper sub-fix:** issue the D2H leg on a source-device stream (a few lines). The microbench gives 1.24x per flow on
serial legs.

## 5. Suggested correction to RANKED

- **Split #4:**
  - **(a) Cross-switch shuffle byte floor** (topology + hash-shuffle design): ≈1.0 s. Lever: broadcast, key co-location,
    fewer bytes. Not pipelining.
  - **(b) Serialized, foreign-stream 2-leg staging** (mechanical): ≈0.6 s (0.5-0.8). Lever: overlapped legs, one sync,
    D2H on the source stream.
- In the roadmap, step 4 is worth ≈0.6 s from #4 plus whatever per-clone latency it removes from #3. It is not 1.4 s
  attributable to #4.
