# PR-5 kit plan/result parity (SF1000 and SF100)

Labels: A = enable_group_join OFF (`../pr4/sirius-groupjoin-off.yaml`), B = ON/default
(`../pr4/sirius-groupjoin-on.yaml`); all runs on the final PR-5 binary (post-clang-format).
Normalization: timestamps / [ids] / file:line stripped; structural lines compared
(`Pipeline #`, Input/Output/Dependencies, plan arrows, GROUP_JOIN, GPU_SCAN).

## SF1000 A vs B (tpch_20260825_235103_gj_pr5_sf1000_A vs tpch_20260825_235539_gj_pr5_sf1000_B)

- Plans: 20/22 identical; only q2 and q17 differ, each by its knob-gated STREAM fusion
  (q17: `Fusing INNER AVG into GROUP_JOIN (STREAM schedule)` + counted port `barrier: PIPELINE`;
  q2: `Fusing DIRECT MIN into GROUP_JOIN (STREAM schedule)`); q13's P0 fusion identical in both.
- Results: 22/22 `result.txt` byte-identical (md5).
- q21/q22 per-query logs in the B suite dir were not carved by the harness (the run crossed
  midnight and the per-day master log rotated); their structural parity was verified from a
  dedicated re-run pair (`gj_pr5_sf1000_A_q2122` / `gj_pr5_sf1000_B_q2122`, see below) and their
  results are byte-identical in the original pair.

## SF1000 PR-5-A (knob off) vs PR-4-B (carried PR-4 minor: the knob-off-vs-HEAD artifact)

Comparison: `tpch_20260825_235103_gj_pr5_sf1000_A` vs PR-4's `tpch_20260825_190445_gj_pr4_sf1000_B`.

- Structural plan lines: 22/22 identical.
- What "identical" means here: at SF1000 PR-4-B declined the q17/q2 fusions (byte gate), so its
  plans are the generic ones -- exactly what PR-5's knob-off planning produces. The raw normalized
  diff flags q2/q17 only because PR-4-B's logs carry its own
  "GROUP_JOIN INNER/DIRECT fusion declined ..." INFO lines (knob-on planner chatter), which a
  knob-off run never emits; every pipeline/wiring/operator/scan line is identical, and every
  result.txt (q1..q22) is md5-equal between the two runs.

## SF100 A vs B (tpch_20260826_000718_gj_pr5_sf100_A vs tpch_20260826_000943_gj_pr5_sf100_B)

- Plans: 20/22 identical; only q2 and q17 differ, each by its knob-gated fusion, and both carry
  the "(one-shot schedule)" selector marker -- every SF100 shape stays one-shot per the selector,
  preserving PR-3/PR-4 behavior (q17: `INNER AVG sparse path` filtered regime; q2:
  `DIRECT MIN sparse path`, state bytes 239997456 / input 1585368 as in PR-4).
- Results: 22/22 byte-identical A vs B.
