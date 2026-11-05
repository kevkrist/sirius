# Verify rank 6: exchange policy (forced P>=N, shuffle instead of broadcast, re-shuffle). Measurement lens

Verdict: **CONFIRMED, with small corrections.** Corrected share is **0.72 s (range 0.60-0.85)** against the claimed 0.76.
The lever claims check out after a small noise correction. Two parts of the narrative are wrong (C1, C2) but do not
move the number much.

Scripts (CPU only, re-runnable) are in `30-diagnosis/verify_policy_meas/`:
- `oracle.py`: timings, oracle, noise and equal-policy gap.
- `forced.py`: forced-class PARTITION thread-time from Quent.
- `pipes.py`: per-pipeline spans per query, iter 3.
- `q3clones.py`: nsys clone counts and bytes in the Q3 iter-2 window.

## Re-derived numbers (all match the claim)

| Claim | Re-derived | Source |
|---|---|---|
| Forced-class PARTITION thread-time: g4 16.0 s vs g1 0.13 s | 16.05 vs 0.128 s (natural class 13.96 vs 6.08) | `forced.py`, Quent g1_a/g4_a, iters 2-4 |
| Q10 P8 (PARTITION(24) through MERGE_TOP_N): 138 vs 143.5 ms | 139.0 vs 142.8 ms (iter 3) | `pipes.py` |
| joinpol g4: 20.04 → 19.07 | 20.039 → 19.068 (−0.971) | `oracle.py` |
| Oracle baseline/joinpol: 18.64 | 18.653 (−1.386) | |
| Oracle on top of batch4g: −0.50 | −0.503 (Q3 −0.358, Q9 −0.052, Q12 −0.035, Q5 −0.030, Q21 −0.018) | |
| Q7 and Q14 regress | Q7 +0.153 and Q14 +0.071 at baseline; +0.161 and +0.224 on top of batch4g | |
| Q3: 1.056 → 0.655 s | −0.401 s (sd 0.004 and 0.010) | |
| Q3 probe: 51.4 GB, of which 25.7 GB staged | nsys2_g4 rep.3 Q3 window: 1440 staged + 720 peer clones (CONCAT(14) accounts for 1254 + 627). Staged D2H legs ≈ 27 GB | `q3clones.py` |
| Code | `partition_sizing.hpp:105-118` floors P at N once the input reaches 16 MB × N. `plan_generator.cpp:1426-1475` always inserts PARTITION(FULL) → MERGE with no partitioning-property check | source |

Every time-based number above is un-profiled. Phase spans come from Quent, which is not profiled. nsys was used only
for counts and bytes.

## Corrections

**C1. Two different "0.76" numbers match by coincidence. They are not one result confirmed twice.**
- The claimed share, 0.763 s, is the `family_map.csv` POLICY sum: JS-F2 260 + JS-F4 39 + JS-F5 129 + SJ-F4 149 +
  MJ-F3 171 + small 15. That is the baseline config.
- "0.76 s at best" is X2's *forced-class exchange excess under the best config*, 757 ms.
  - It includes allocator and clone share.
  - It covers a different set of queries: Q21 313, Q10 220, Q4 66, Q18 59, Q22 23, Q17 19.
  - Q3, Q5, Q8, Q9 and Q12 are absent from it, because broadcast already fixed them.
- So "excess 2.40 → 1.17 → 0.76" does not support the 0.76 share.

**C2. The bucket is not the "non-allocator, non-clone remainder".**
- JS-F2 (Q3, 260 ms) is the P4 phase gap minus the PARTITION allocator time. That phase is dominated by CONCAT(14)
  clones: 5879 GPU-origin prepare thread-ms, 95% of CONCAT task time, 25.7 GB staged.
- JS-F5 (Q10 re-shuffle, 104 of 129 ms) is dominated by MERGE_GROUP_BY clone prepare: 690 of 880 thread-ms, with
  4.04 GB staged.
- So **≈0.36 of the 0.76 s is clone or staged-byte time, assigned here by root cause.**
- This is not a double count, because `family_map` books each item once. But the narrative should say "policy-caused
  exchange cost, net of the allocator". #4 lists Q3 among its top queries even though no Q3 bytes sit in #4's waterfall
  amount.

**C3. The allocator split inside forced PARTITION used profiled ratios: +0.05 to +0.09 s to this bucket.**
- The non-allocator parts (JS-F4 39, MJ-F3 171, SJ-F4 149, small 15 = 374 ms) are the un-profiled phase gap minus an
  allocator share. That share came from nsys fractions or latencies:
  - 75-80% of PARTITION time;
  - 66.7 µs per malloc profiled, vs 56 µs un-profiled in the microbench at 4x4.
- CUPTI inflates the allocator calls more than the rest, as RANKED itself notes with its ×0.75 scaling.
- Rescaling gives a non-allocator share of ≈69% instead of 75-79%, or (56−6.7)/(66.7−6.7) = 0.82 of the MJ allocator
  counterfactual. That moves ≈+10 (JS), +48 (MJ) and +30 (SJ) ms from #2 into #6.

**C4. Join-mode confound on Q3: −0.08 s under equal-policy accounting.**
- At g1, Q3 runs STANDARD because of the 2 GB `max_build_hash_table_bytes` cap.
  - joinpol speeds g1 Q3 up by 0.867 s (2.031 → 1.164).
  - So the baseline ideal for Q3 is inflated by 0.217 s.
- With equal policy at both counts:
  - The Q3 gap moves from 0.548 to 0.364. Policy's share is 0.184 s, not 0.260.
  - Suite gap: blanket −0.70 s (10.38 vs 11.08), not −0.97. Per-query oracle −1.17 s, not −1.39.
- RANKED keeps the fixed g1 anchor and credits the join confound only −0.02 (#11). Under that convention 0.26 is
  defensible: the g4-only removal is 0.40.
- The claim's own remedy says to report scaling with equal policy, so I take the equal-policy value.

**C5. The oracle carries a selection bias of about 0.03-0.09 s.**
- Null test: a per-query minimum of g4_a and g4_b, which have the same config, gives 20.007. That is −0.032 vs g4_a,
  and −0.085 vs the mean of the two runs.
- The per-query session noise at g4 reaches 0.046 s (Q18).
- So the baseline oracle is ≈−1.30 s, and the oracle on top of batch4g is ≈−0.46 s. In the second case, Q21 −0.018
  and Q5 −0.030 are at noise level.

**C6. Unmeasured parts, about 0.35 s, rest on attribution alone.**
- The affected entries:
  - Q10 F4 non-allocator part;
  - Q10 F5 re-shuffle, 104 ms;
  - Q4 38 and Q21 49 (SJ-F4);
  - the group-by P≥N floor.
- joinpol does not flip any of these joins (Q4 −0.001, Q10 +0.007, Q21 −0.018), and the gate cause is marked
  "unverified".
- The Q10 re-shuffle is priced as g4 P8 − g1 P8/4. That assumes a path with no re-shuffle reaches g1/4. g1 P8 itself is
  merge-heavy (PARTITION 209 + MERGE 318 thread-ms over 9 partitions), so this is plausible but not measured.

**C7. Possible overlap with #9 (Bloom), at most about 0.03 s.**
- MJ-F3 includes "small build-side exchanges" (16-24 ms per query) in the same pipelines that hold the accumulated-Bloom
  finish. That finish is booked separately in MJ-F5.
- Joinship and semijoin exclude Bloom explicitly.
- The 16.05 s forced-class thread-time includes Bloom tails, for example Q10 PARTITION(11) at 57 thread-ms with 41 ms of
  publication. That figure is evidence only and is not part of the price.

## Corrected estimate

0.763 − 0.08 (C4) + 0.07 (C3) − 0.03 (C7) ≈ **0.72 s (6.5% of 11.08)**.
- Range 0.60-0.85.
- About 0.35 s of it is unmeasured (C6).

Levers, corrected:
- At baseline: blanket −0.97 s (g4-only) or −0.70 s (equal-policy gap); oracle ≈−1.30 / −1.17.
- On top of batch4g: oracle ≈−0.46, blanket 0.00.
- Residual forced exposure at best: 0.76, of which ≈0.4-0.6 is recoverable. This rests on the X2 class definition and
  includes allocator and clone cost. It is not measured.

## Checks with no issue found

- Window and iteration: Quent iters 2-4 averaged. Q10 was checked at iter 3. The nsys Q3 window is restricted to the
  map.csv iter-2 range.
- Thread-time vs wall: the family prices are critical-path phase spans. X2 uses ∫1/n_active, validated against joinpol
  within about 5%.
- Profiled vs un-profiled: all time prices are un-profiled. Only the splits use nsys (C3).
- Q13 cold: Q13 is not in this bucket, and joinpol leaves Q13 unchanged (+0.008).
