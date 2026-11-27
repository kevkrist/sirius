# STATUS 2026-11-27 UTC (session resumed after NVMe wipe)

- Restored: tip build, SF1000 data, notes, fork branches (no worktrees except those agents recreate).
- NEW: notes/DESIGN_TRANSFER_PR.md (architect, transfer PR production design; open questions for user inside).
- RUNNING: workflow df-close-out (eager finish + verify, combined measure + verify, DECISION.md).
  Script: ~/.claude/projects/-home-ubuntu-sirius/90fe0da0-364a-41dc-b0f4-acbde346fc58/workflows/scripts/df-close-out-wf_a708a68d-82c.js
- Below: state as of 2026-11-20 (still valid).

# STATUS 2026-11-20 04:10 UTC

Baseline: base 15225 ms, tip (#2570) ~15300-15370 ms, DF off 16627 ms.

| Track | Bucket | State | Suite gain vs tip |
|---|---|---|---|
| D2D inside pairs + host hop between pairs (`proto/xfer-d2d`, `SIRIUS_PROTO_XFER_D2D=pipe`) — USER'S LEAD DESIGN | transfer PR | prototype VERIFIED (notes/VERIFY_PROTO_XFER_D2D.md) | -370..-410 ms (-2.4..-2.7%) |
| All-host pipelined bounce, CPU OR (`proto/xfer-host`, `auto`) | transfer PR (cross-pair leg / no-P2P fallback) | prototype VERIFIED (notes/VERIFY_PROTO_XFER_HOST.md) | -394 ms (-2.6%) |
| Shadow rule (`prod/shadow-2569` 8e049ef7, `prod/shadow-2570` cd89bbc8) | #2569 + #2570 | architect -> developer -> reviewer -> fixes DONE (DESIGN/IMPL/REVIEW_SHADOW_RULE.md). Awaits user decision to apply to PRs | -343 ms (-2.2%); q21 -306, q8 -35 |
| Eager scans S6 (`proto/eager-probe`, `proto/eager-opt2`) | S6 PR | option 1 measured (-295 ms); section 6 (throttle/gate variants) stalled 01:40 when session died; NOT verified | -295 ms (unverified) |

Gains overlap (shadow rule removes q21's big filter that transfers speed up): combined number NOT measured yet.

## Next steps
1. Finish PROTO_EAGER section 6 from raw data in runs/eager (+ template notes/PROTO_EAGER_sec6_template.md); verify eager.
2. Combined branch (shadow-2570 + xfer-d2d + eager-probe), full suite ABBA vs tip and vs base.
3. notes/DECISION.md: per-bucket ledger.
4. Transfer PR + S6 PR production code: architect -> developer -> reviewer.
5. Open: ncu denied for agents; results not checked vs DuckDB CPU; hooks disabled in sirius/.claude/settings.local.json.
