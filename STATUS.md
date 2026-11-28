# STATUS 2026-11-28 01:17 UTC

Final synthesis: `notes/DECISION.md` (read it first; this file is its short form).

Baseline (11-18 reference): base 15225 ms, tip (#2570) 15372 ms (15290-15320 in later sessions), DF off 16627 ms.

| Piece | Bucket | State | Suite gain vs tip (verified) |
|---|---|---|---|
| Shadow rule (`prod/shadow-2569` 8e049ef7, `prod/shadow-2570` cd89bbc8) | #2569 + #2570 | reviewed production code, fixes done; awaits user decision to apply to the PRs | -343 ms (-2.2%); q21 -306, q8 -35 |
| Transfer: D2D inside pairs + host hop (`proto/xfer-d2d` b7d0eb81, `pipe`) - USER'S LEAD | transfer PR | prototype verified; production design `notes/DESIGN_TRANSFER_PR.md`; no production code | -370..-410 ms (-2.4..-2.7%) |
| Transfer: all-host pipeline, CPU OR (`proto/xfer-host` 699b3dd8, `auto`) | transfer PR (cross-pair leg = no-P2P fallback) | prototype verified | -394 ms (-2.6%) |
| Eager scans, every join + gate 2 (`proto/eager-opt2` fdd2caec, "f1"); filter joins only (`proto/eager-probe` f05cfd12) | S6 PR | prototype verified (`notes/VERIFY_PROTO_EAGER.md`); design text only (PROTO_EAGER 6.8); admission / re-evaluation / notification outside locks unbuilt | -497..-512 ms (-3.3%); filter joins only -288..-324 |
| Combined shadow + xfer-d2d + eager (filter joins only) (`proto/combined` c5aa5bdf, measurement branch) | all | measured + verified (`notes/COMBINED.md` section 10) | **-786 ms (-5.1%)** (author -830); = base - 639 ms (-4.2%); DF on/off ~1.14x |

Overlap: shadow rule and transfer share ~210 ms (q21 j18, q8 j41). Eager is additive to both.
Not measured: combined with the every-join eager shape (estimate ~-970 ms).

Killed: OP1A/B, single-GPU build, hybrid one-shot (verification still running 11-28 01:15, `runs/vhybrid`, no verifier
note yet), OP4, OP5, option 2, gate 1, throttle K>=2, batched staging, replica relay, all kernel variants.
One line each in DECISION.md section 3.

## Open for the user (DECISION.md 4)
1. Apply shadow-rule branches to #2569 / #2570 (the #2570 one is a rebase = force-push).
2. TPC-H results never validated against DuckDB CPU (only identical to tip).
3. ncu permission denied for agents.
4. Hooks disabled in `/home/ubuntu/sirius/.claude/settings.local.json` (`disableAllHooks: true`).
5. Transfer design Q3, Q5, Q7, Q9 unanswered. S6 scope: joins without a filter, gate 2 vs issue text, throttle.
6. Landing order: #2570 alone is 1.0% slower than base.

## Next steps (DECISION.md 5)
1. Collect the hybrid one-shot verification.
2. Transfer step 0: spike `proto/xfer-compose`, decides CPU OR vs GPU OR between islands (Q1).
3. Transfer PR production code: developer -> reviewer (`notes/IMPL_TRANSFER_PR.md`).
4. S6: architect document, then developer -> reviewer; add the "PARTITION finalize => session Terminal" contract to #2570.
5. Measure combined with the every-join eager shape; repeat with production branches.
6. Shadow rule: sanitizer + per-commit builds; apply when the user says so.
7. Validate TPC-H against DuckDB CPU once.
