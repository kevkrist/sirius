# S4 experiment branch and recovery record

Published for the user's four-GPU agent on 2026-10-23. Start with
[S4_REMOTE_EXPERIMENTS.md](S4_REMOTE_EXPERIMENTS.md). Return the report and raw
evidence on `kevkrist/sirius`, branch `experiment/s4-four-gpu-20261023`.

## Reachable prerequisites

The parent merge commit is `78a3e34c66143152ff075729f4c018806c11e67d`. Its tree is
the previously validated prerequisite tree
`9f59d68e4cdc7ca47999c3579407f6f5168ea652`; its parents are:

- S3c / PR #2280: `9c329e31f65be938bf9de4fc823bd29abf0359cc`.
- cuCascade migration / actual PR #2302: `cb6e7f9d0a055679b700e30f2da51b1ba331a027`.

The next commit contains the recovered S4 implementation, its design/review/run
documents, and these remote instructions. Record the checked-out source commit
before building. The prerequisite tree is reachable from this history, so the
build helper's manifest baseline works in a fresh clone. Both schedules must be
built from the same frozen source commit and dependency state.

## Recovery and validation scope

The original implementation worktree and evidence archives were under `/tmp` and
were absent when the publication task began. Persistent Git metadata retained the
prerequisite index. The parent and original agents recovered their final files by
replaying successful source edits and formatting from their session records.

Before creating publication commits, the recovered production/fixture provenance
hash matched both original validated build manifests exactly:

```text
57f35e0ab8fac84b353387be33432641d5477f773287f58286abdf1b05d4eb9e
```

This is `source_identity` from `scripts/run_s4_experiments.py`: the prerequisite
tree, original S3c HEAD, tracked source diff, untracked implementation hashes, and
recursive submodule status. Its scope includes CMake, `src`, and `test/cpp`; Python
scripts and root Markdown files were recovered separately from their recorded
writes. Publication commits change HEAD and tracked-file status, so newly built
manifests will have a different source identity. Compare the new paired manifests
to each other; do not require this historical hash on the committed branch.

The recovered Python harness passed its 20 CPU tests again. The recovered source
passed formatting/lint checks. Historical GPU results remain those documented in
[S4_VALIDATION.md](S4_VALIDATION.md) and [REVIEW.md](REVIEW.md): both schedules passed
94 focused cases and 7 fault cases; six bounded sanitizer runs reported zero
errors; the local paired trial produced 100 passing samples from 50 processes.
Those were two-GPU checks, and the tiny SQL input produced no accumulated publication.

No new GPU run is claimed for recovery/publication. Historical `/tmp` log/archive
links are unavailable in the restored environment. The remote agent must build,
verify correctness, and collect fresh four-GPU evidence as instructed. Final
topology selection and production performance acceptance remain pending.

## Local review state

Publication uses a separate worktree. The recovered review worktree is
`/home/ubuntu/sirius/build/s4-publication`, branch `stacked/df/04-publication`.
It retains the staged prerequisite tree and unstaged/new S4 changes. The user's
original `/home/ubuntu/sirius` worktree and changes remain untouched.

Statements about the uncommitted worktree in older design/validation documents
describe that review state. This experiment branch intentionally commits the
same source so it can be transferred and results can be pushed back.
