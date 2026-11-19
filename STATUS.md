# STATUS 2026-11-19 23:15 UTC

Workflow `df-continue` running (3 tracks). Nothing verified/final yet.

- Baseline: base 15225 ms, tip (#2570) 15372 ms (+1%), DF off 16627 ms. Gap = filter finish on critical path (600 ms/suite, mostly host bounce).
- Transfer PR (lead design per user: D2D inside pairs + host hop between pairs = `proto/xfer-d2d`, `SIRIUS_PROTO_XFER_D2D=pipe`):
  finish 186 -> 52 ms/GiB; 7 queries (q3,4,7,8,9,10,21) -364 ms (-5.7%), interim 1 ABBA block, results identical.
  All-host CPU-OR (`proto/xfer-host`, `auto`): 45-58 ms/GiB, q21 -208, q3 -41. Verifiers pending.
- S6 eager scans (`proto/eager-probe`, `proto/eager-opt2`): suite -295 ms earlier; re-run q3 -44, q8 -39, q21 -64; scan throttle
  helps q3/q8 more (-61/-72). Build move's host->GPU copies queue behind scan copies (84 ms/buffer). Full suite + verifiers pending.
- Shadow rule (PR-bound, #2569/#2570): design done; implemented on `prod/shadow-2569`, `prod/shadow-2570`; tests/measure in
  progress; reviewer pending.

## Next steps
1. Finish/verify each track; write `notes/DECISION.md` with per-bucket ledger.
2. Combined measurement: eager + D2D transfers + shadow rule, full suite.
3. Transfer PR and S6 PR: architect -> developer -> reviewer on the verified design.
4. Open: ncu denied for agents; results not checked vs DuckDB CPU; hooks disabled in sirius/.claude/settings.local.json.
