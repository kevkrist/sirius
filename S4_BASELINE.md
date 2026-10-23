# S4 review baseline

Worktree: `/tmp/sirius-s4-publication`; branch: `stacked/df/04-publication`.

- S3c / PR #2280: `9c329e31f65be938bf9de4fc823bd29abf0359cc` (HEAD).
- cuCascade migration / actual PR #2302: `cb6e7f9d0a055679b700e30f2da51b1ba331a027`.
- Migration parent: `12d312ee02263f0187bf4c91ffd689e8e5d48229`.
- Combined prerequisite tree: `9f59d68e4cdc7ca47999c3579407f6f5168ea652`.
- cuCascade gitlink: `1a9281093a0fe49634594d541d7ce327129f53d6`.
- Optimized integrated reference: `ef17c8c55003dbc889e93c24eedf55f127f22c7a`.

The prerequisites merged without conflicts. No merge or implementation commit was created.
`git merge --quit` removed merge bookkeeping after preserving the combined prerequisite
index. Keep that index frozen: staged changes are prerequisite integration; unstaged and
untracked changes are S4. Review tracked S4 changes with `git diff`; include new S4 files.
`git diff 9f59d68e4cdc7ca47999c3579407f6f5168ea652` also compares tracked files to the
combined prerequisite baseline. Do not compare only to HEAD and attribute prerequisite
changes to S4.

The original `/home/ubuntu/sirius` worktree and its user changes are preserved.
Four-GPU topology selection remains pending user measurements.
