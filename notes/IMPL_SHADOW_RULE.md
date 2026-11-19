# IMPL_SHADOW_RULE: production implementation of the shadow rule (design: DESIGN_SHADOW_RULE.md)

2026-11-19. STATUS: IN PROGRESS (this header is rewritten when the work is finished).

Handoff state (for a successor if this agent stops):
- Worktrees: `R/wt-prod-69` (branch `prod/shadow-2569`, off pr2569 a2d6bd5a), `R/wt-prod-70` (branch `prod/shadow-2570` =
  pr2570's 7 commits rebased on prod/shadow-2569 + wiring + late abandon + docs). Both pushed by backup.sh under dfperf/prod/*.
- Scripts: `R/runs/shadow/bin/{build,unit,one_query,abba,dbg}.sh`, `abba_table.py`. Output `R/runs/shadow/`.
- Code written for all 6 commits; first build of wt-prod-69 running; nothing tested or measured yet.
- Next: fix compile errors, run DF unit tests on both branches, rebase prod/shadow-2570 on the final prod/shadow-2569,
  build wt-prod-70, 22-query result check, DEBUG log pass, ABBA q8/q21/q3/q9/q10/q7, suite.
