# Verify rank 6 "exchange-policy-forced-partition-broadcast" (causality lens)

Verdict: **OVERSTATED (mildly)**. Corrected primary share **0.65 s** (range 0.55-0.70), not 0.76. Rank 6 stands.
The remedy and lever text has bigger errors than the share does.

Scripts: `30-diagnosis/verify_policy/{oracle.py,part_split.py,explain.py}`. All ran on the CPU; no GPU was used.

## 1. Code mechanism: confirmed

- `partition_sizing.hpp:103-120` (`natural_num_partitions`) floors P at N once the input is at least 16 MB × N.
  The constant is at `sirius_config.hpp:76`.
- `sirius_physical_hash_join.cpp:873`: BUILD_PROBE runs at `min(natural, N)`.
- `sirius_physical_partition.cpp:158`: `count < 2` means pass-through. So at N=1 a BUILD_PROBE exchange is free.
- `:867-869`: broadcast candidacy.
- `:889`: BUILD_PROBE requires `!is_right_family`, and `:891` makes broadcast require BUILD_PROBE.
- `sirius_physical_plan_generator.cpp:1425-1475` (`wrap_hash_group_by`) always inserts PARTITION(FULL) → MERGE. It has
  no partitioning-property check.

## 2. Claimed pieces (`family_map.csv`, POLICY = 763 ms) and their causal status

| piece | ms | what it really is | causal verdict |
|---|---|---|---|
| JS-F2 Q3 probe shuffle | 260 | Cost of moving 38.5 GB of the probe (25.7 GB host-staged, CONCAT(14) clones). This is **clone/staging cost, not "non-clone"**. At g1 the probe is also hash-partitioned (STANDARD, 54-way), so the partitioning is not new work. Only the movement is new. | **Keep.** Robust: still −0.358 s ΔT4 after batch4g. Two causes: broadcast gate **and** the 256 MB per-GPU Bloom cap, which refuses 294 MB at both g1 and g4. Either one alone removes most of the probe. |
| JS-F5 group-by re-shuffle (Q10 104, Q3 25) | 129 | The 9-way re-partition exists at g1 too (8.1 GB / 1 GB), so it is not forced. What is new at g4 is moving 6 GB of partials (36 staged clones at 18.6 ms each). That is **clone/staging cost** as well. | **Keep.** Mechanism confirmed in code. Persists under `best` (joinship Rev2 R6 −85). |
| MJ-F3 non-alloc (Q5/Q8/Q9) | 171 | Per-task fixed cost × 627 tasks | Keep, with a caveat (§3). joinpol removes it. |
| SJ-F4 Q12 part | 62 | same | Keep. joinpol: PARTITION(2) 139 → 5 ms. |
| SJ-F4 Q4 + Q21 parts | 38 + 49 | same | **Not policy-fixable** (§4). Move to #3 (FRAG). |
| JS-F4 non-alloc (≈Q3 8, Q10 31) | 39 | same | Q3 part flips with broadcast. **Q10 part (≈31) is not policy-fixable.** |
| small-F3 non-alloc | 15 | small group-by inputs forced to P=4 | Keep. Dropping the floor fixes it. |

**Corrected policy-causal share** = 260 + 129 + 171 + 62 + 8 + 15 = **645 ms**. Reassigning about 118 ms (Q4 38,
Q21 49, Q10 ≈31) from POLICY to FRAG (#3) leaves the waterfall total unchanged.

## 3. Discriminating check: is the "non-alloc remainder" real, or allocator collateral?

`part_split.py` runs on nsys2_g4 iter2 and looks at the forced probe PARTITION ranges.

| | ranges | thread-ms | alloc API | non-alloc | max per-thread non-alloc | kernels |
|---|---|---|---|---|---|---|
| Q5 P25 | 627 | 2178 | 1762 (81%) | 416 | 30.5 ms | 20.6 ms |
| Q8 P34 | 627 | 2628 | 2176 (83%) | 452 | 36.4 ms | 21.5 ms |

- **Uncontended counterfactual for the phase:**
  - per-thread non-alloc time (≈30-36 ms);
  - plus alloc time at g1 per-call cost (≈0.18 × alloc / 16 ≈ 20-25 ms);
  - total ≈ 50-60 ms profiled, ≈ 40-50 ms un-profiled.
- That matches what MJ booked as non-alloc (Q5 46, Q8 55). So the split is quantitatively fine.
- `cudaMemcpyBatchAsync` mean latency is flat between g1 and g4 (24.3 vs 22.7 µs, `api_latency_compare` on Q8 + Q12).
  So the remainder is **not** hidden allocator write-lock collateral.
- **Caveat: it is mostly per-task fixed cost.** Kernels are only about 21 ms per 627 tasks.
  - batch4g (no policy change) removes 80-95% of it: MJ-F3 441 → 28; Q21 PARTITION(30) 75 → 3.3 ms; X2 forced
    PARTITION share 1335 → 519 ms.
  - So it is a policy × granularity interaction. As a primary baseline attribution it can stay with POLICY. It is not
    an independent lever once #3/batch4g is applied.

## 4. Remedy and lever claims refuted or overstated

- **"Fix the gates that keep the Q10/Q21/Q4 joins shuffled"** and **"0.4-0.6 s of residual forced partitioning"**
  are not credible.
  - CPU EXPLAIN (pixi duckdb 2.0.0, parquet stats, `explain.py`) gives these join types:
    - Q4 is a **RIGHT_SEMI** (orders 60M rows ⋈ lineitem 240M rows).
    - Q21 is **RIGHT_SEMI** and **ANTI**, lineitem ⋈ lineitem (1.2B rows each side).
  - `:889` excludes the right family from BUILD_PROBE, so from broadcast too. Broadcasting a right-semi build needs a
    cross-GPU match-flag reduction, which is a design change, not a gate tweak.
  - The lineitem ⋈ lineitem builds cannot be replicated anyway.
  - The semijoin family already concluded that "Q4/Q21 need co-partitioning". joinpol: Q4 −1 ms, Q21 −18 ms.
  - Q10 HJ(10): joinship R2 says runtime broadcast saves "about 0" after batch4g.
  - The residual forced excess at `best` is 757 ms, of which Q21 313 + Q10 220 = 70%. That is data-placement / FRAG
    territory, not policy.
  - Realistic recoverable residual by policy: about 0.05-0.15 s (small group-by floors in Q18/Q22/Q17).
- **The two Q10 remedies conflict.**
  - Broadcasting the customer join (F6, 77 ms) makes the probe local and un-partitioned on c_custkey.
  - The group-by re-shuffle (F5, the one that property propagation would skip) then becomes necessary.
  - So the two cannot both be credited.
- **Lever after batch4g** (`oracle.py`: hot means, iters 2-4):

  | case | Δ (s) |
  |---|---|
  | blanket, ΔT4 | +0.012 |
  | oracle, ΔT4 | −0.503 |
  | of which Q3 | −0.358 |
  | all other queries together | ≈ −0.15, several within noise |
  | oracle, equal-policy Δgap (g1 also on joinpol) | **−0.264** |

  - At g1, Q3 flips from STANDARD to BUILD_PROBE (2.031 → 1.164 s).
  - At baseline: blanket −0.971 ΔT4 / −0.701 equal-policy; oracle −1.386 / −1.157.
- **The evidence text conflates two different 0.76 numbers.**
  - The X2 "forced excess at best" is 757 ms. It includes clone and allocator time and is dominated by Q21/Q10.
  - The family POLICY sum is 763 ms. That is coincidence, not corroboration.
- The bucket definition "non-allocator, non-clone remainder" is wrong for 389 of the 763 ms (F2 + F5 are clone/staging
  cost). This does not double-count: family_map has no Q3-P4 or Q10-P8 rows in FRAG or BYTES. But the label misleads
  remedy sizing, because the #4 pipelined-staging remedy also shrinks these: X2(B) Q3 −196, joinship R5 Q10 P7 + P8
  217 → 120.
- Confidence differs between the claim ("medium") and the RANKED table ("medium-high"). **Medium** is right.

## 5. Corrected numbers

- Primary share at baseline: **0.65 s (5.8% of the 11.08 s gap)**, range 0.55-0.70.
- Policy-only lever on top of batch4g:
  - ≈ 0.35-0.6 s ΔT4, dominated by Q3. The Q3 broadcast or the Bloom-cap raise are alternative fixes.
  - Plus about 0.08 s from property propagation for Q10.
  - Equal-policy gap closure is only about 0.3-0.4 s.
- Remedies to keep:
  - cost-aware broadcast (Q3/Q5/Q8/Q9/Q12; avoids the Q7 +0.16 and Q14 +0.22 regressions at batch4g);
  - raise the per-GPU Bloom cap (Q3, unmeasured);
  - property propagation (Q10);
  - no P≥N floor for small group-by inputs.
- Remedy to drop: "fix gates for Q4/Q21/Q10". Q4/Q21 need right-family distributed semantics or orderkey co-placement,
  which belongs under #4.
