# Verify rank 1 "host-link-floor" — measurement lens

Verdict: **OVERSTATED (mildly)**. Mechanism and hardware fact hold. Priced share 3.48 s → **≈2.9 s (range 2.75-3.4)**.
Hardware-remedy lever ("per-GPU uplink ≈3.5-3.8 s") → **≈2.4 s**.

Scripts/data (CPU only): `30-diagnosis/verify_hlf/link_rederive.py` → `link.csv` (own per-query H2D/D2H bytes and
per-switch busy union, nsys2_{g1,g4} iter 2, restricted to map.csv windows, classified by srcKind/dstKind);
`30-diagnosis/verify_hlf/floor_check.py` (floors, variants, gpupin bounds, counterfactual).

## Re-derived (agrees with claim)
- Bytes: g1 H2D 1254.0 GB (Q13 incl. 14.25 GB D2H/re-upload at g1 → scan 85.6); g4 H2D 1367, D2H 124, scan est
  (H2D−D2H) 1249. Switches balanced (687 GB busier-switch sum). PtoP-labelled pinned→device copies confirmed
  (rep.2: kind 10 srcKind 1 = 13.1 GB vs kind 1 = 17.5 GB).
- Busier-switch busy (union) raw: g1 26.3 s, g4 13.1 s; rate while busy 47.7 / 52.5 GB/s; busy frac 71% / 60%.
- Q17/Q19/Q20: speedup 2.20/2.18/2.10x; busy 94→95%, 90→91%, 93→91%. Confirmed.
- Floor Σ max(0, min(B_busier_sw/52, T4) − T1/4) = **3.41 s** (claim 3.48; small diff from scan-share split). gap 11.08.
- gpupin clean-19: 16.335 → 12.513 (−3.82). batch4g link-bound Q2/11/17/19/20 unchanged (±0.02). g2x→g4 link-bound
  3.511 → 3.192. batch4g floor 3.95 of 8.65 gap (claim 4.01). All reproduced.
- Pure wall-time construction; no thread-time conversion involved. Iteration/window use correct (iter 2, map windows).

## Problems found
1. **"Hard" floor uses a rate the engine already beats.** Floor assumes 52 GB/s/switch (microbench: one 1 GiB
   cudaMemcpyAsync per GPU, includes thread-start skew). Engine measures, per busier switch while busy:
   Q2 55.5, Q11 55.4, Q17 54.8, Q20 54.6, Q13 54.4, Q8 54.0, Q19 53.4 GB/s at g4; and a SINGLE GPU at g1 reaches
   55.3 (Q2) / 55.5 (Q11) on uncompressed `cudaMemcpyBatchAsync` tables (dmon g1 rx max 59.9). So hardware cap ≥55.5.
   Floor sensitivity: 52 → 3.41; 52.5 → 3.31; 54 → 3.02; 55 → 2.83 s. A hardware floor must use the demonstrated
   cap → **≈2.8 s**. The difference (~0.6 s) is software: g1 compressed scans with 1 MiB blocks
   (`compressed_representation.cpp:62-86`) reach only 45.8-48.8 GB/s per GPU vs 55 for batched raw copies.
2. **Q13 g1 cold confound runs the other way (understates).** g1_a Q13 hot mean 3.011 (iters 2-4) vs 2.557 (iters 3-4).
   Using warm Q13: floor +0.11 (3.41 → 3.53 at 52; 2.83 → 2.94 at 55).
3. **gpupin is not a check on the gap share.** It removes the link entirely (infinite BW), so it also removes the
   g1-equivalent link time: 6 clean queries end BELOW T1/4 (Q2, Q11, Q13, Q17, Q19, Q20; 0.67 s below ideal total).
   It also changes prepare path (no host source buffers) and adds GPU memory pressure (9675 downgrade convert errors;
   "clean" Q10 +0.37 s, Q21 +0.04, Q16 +0.03 slower). Σ min(saved, gap) on clean 19 = 3.62 — coincidentally near
   3.41, but two confounds of opposite sign; treat as loose upper bound on a ΔT4 lever, not confirmation of share.
4. **Hardware-remedy lever overstated.** One uplink per GPU halves link time; it does not remove it. Model:
   T4' = max(T1/4, T4 − s_q·busy_q/2), s_q = gpupin saved / scan-copy time (clipped 0-1; 0.40 for Q1/3/18),
   busy scaled ×0.92 → **ΔT4 ≈ 2.39 s**. Cross-check g2s→g2x (N=2, separate uplinks): sensitivity 0.61 →
   0.4-0.6 × ~5.5 s saved link time = 2.2-3.3 s. So "removes ≈3.5 s" / "gpupin bound −3.8" should read ≈2.4 (2.2-3.3).
5. **2.13x link-busy speedup is partly a scaling artifact.** Claim scales DMA busy by wall factors 0.97 (g1) and 0.92
   (g4). DMA time is not slowed by CPU-side profiler overhead; raw busier-switch busy 26.3 → 13.1 s = 2.01x
   (scan-only ~2.2x). Cosmetic; does not change the floor.
6. Double counting: floor excludes staging bytes (#4) — OK. But #5 LINK_SERIAL = families 4.68 − floor. If the floor
   drops to ~2.9 with the same families' 105 GB/s phase pricing, LINK_SERIAL would absorb +0.5 s and exceed X3's own
   Δgap bound (1.65). The families' link pricing used the same 105 GB/s and is overstated by the same rate error;
   the freed ~0.5 s belongs to a software "1 MiB copy inefficiency / link-efficiency" or back into residual.

## Corrected estimate
Share (ideal-engine floor at demonstrated 55-55.5 GB/s/switch, Q13 warm): **2.85-2.95 s → 2.9 s (26% of gap)**;
range 2.75 (55.5, cold Q13) to 3.4 (52 GB/s). Still rank 1 (next: allocator 1.80). Confidence in the mechanism: high;
in the 3.48 figure: medium. Lever of the hardware fix: ≈2.4 s.
