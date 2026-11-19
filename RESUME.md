# RESUME — DF perf campaign (stack sirius-db/sirius #2569 -> #2570), start here in a fresh session

Auto-checkpointed every 10 min to fork kevkrist/sirius: this branch (`dfperf/notes`) + code branches `dfperf/{proto,mb,prod}/*`.

## Restore (NVMe work dir is wiped on instance stop)
1. `git clone -b dfperf/notes git@github.com:kevkrist/sirius.git ~/dfperf-durable` (if ~/dfperf-durable is gone).
2. Follow `README.md` here ("restore procedure"): run setup (clone, fetch pr2569/pr2570, build, SF1000 data gen ~265 GB)
   into R=/opt/dlami/nvme/dfperf; `cp -r notes runs $R/`; fetch fork branches `dfperf/*` back as local `proto/* mb/* prod/*`.
3. Uncommitted agent work at last checkpoint: `patches/wip/<worktree>.patch` (first line = HEAD + branch it applies to).
4. Raw captures (nsys, Quent, large logs) are NOT backed up; tables in notes are the record.

## Rules
- Config `/home/ubuntu/sirius/specific_config.yaml`: never alter. Builds/GPU runs under `flock $R/machine.lock`.
- PR-bound code: architect -> developer -> reviewer agents. Prototypes: measure first.
- Never push to sirius-db, never open PRs unasked. No agent co-author trailer.
- User decisions: top of `notes/OPPORTUNITIES.md` (authoritative).

## State — see STATUS.md (rewritten at each checkpoint by the orchestrator) and notes/
- Ledger buckets: #2569, #2570, S6 PR (eager probe scheduling, issue #1825), transfer PR.
- Per-track notes: `notes/PROTO_EAGER.md`, `notes/PROTO_XFER_D2D.md`, `notes/PROTO_XFER_HOST.md`,
  `notes/DESIGN_SHADOW_RULE.md` + `notes/IMPL_SHADOW_RULE.md`; final synthesis `notes/DECISION.md` when it exists.
