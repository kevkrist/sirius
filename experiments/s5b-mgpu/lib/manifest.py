#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""The guard of `bin/cleanup.sh`: it deletes only paths the kit recorded as created.

`env.sh` `s5b_record_created` appends one `<kind>\\t<absolute path>\\t<time>` line to
`experiments/s5b-mgpu/.created-paths` whenever the kit creates a directory that did not exist:
the raw-output directory `S5B_WORK` (kind `work`, which also gets a `.s5b-work` sentinel file) and
the build worktrees (kind `worktree`). A path is deletable only when it is recorded with that kind,
is an existing directory and not a symbolic link, carries the sentinel when it is `work`, and is
not, and does not contain, any protected path: `/`, `$HOME`, `S5B_HOME`, the control checkout, the
TPC-H data, the results directory and, for `work`, the worktrees. The TPC-H data is never deleted.

    python -I lib/manifest.py check-delete <work|worktree> <path>   # exit 0 when deletable

Standard library only.
"""

from __future__ import annotations

import os
import pathlib
import sys

KINDS = ("work", "worktree")
WORK_SENTINEL = ".s5b-work"
MANIFEST = pathlib.Path(__file__).resolve().parents[1] / ".created-paths"


def read_manifest(path: pathlib.Path) -> set[tuple[str, pathlib.Path]]:
    if not path.is_file():
        return set()
    entries = set()
    for line in path.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0] in KINDS:
            entries.add((parts[0], pathlib.Path(parts[1])))
    return entries


def protected_paths(environ: dict[str, str], kind: str) -> list[pathlib.Path]:
    names = ["HOME", "S5B_HOME", "S5B_CONTROL", "S5B_DIR", "S5B_DATA", "S5B_RESULTS"]
    names += [name for name in environ if name.startswith("S5B_TPCH_SF")]
    if kind == "work":
        names += ["S5B_WT_tip", "S5B_WT_key", "S5B_WT_base"]
    paths = [pathlib.Path("/")]
    paths += [pathlib.Path(environ[name]) for name in names if environ.get(name)]
    return [pathlib.Path(os.path.realpath(p)) for p in paths]


def delete_problems(
    kind: str,
    target: pathlib.Path,
    recorded: set[tuple[str, pathlib.Path]],
    protected: list[pathlib.Path],
) -> list[str]:
    """Why @p target must not be deleted as @p kind; empty when it may be."""
    if kind not in KINDS:
        return [f"unknown kind {kind!r}"]
    if not target.is_absolute():
        return [f"{target} is not an absolute path"]
    if target.is_symlink():
        return [f"{target} is a symbolic link"]
    if not target.is_dir():
        return [f"{target} is not an existing directory"]
    real = pathlib.Path(os.path.realpath(target))
    problems = []
    if (kind, target) not in recorded and (kind, real) not in recorded:
        problems.append(
            f"{target} is not recorded as a {kind} the kit created ({MANIFEST.name})"
        )
    if kind == "work" and not (real / WORK_SENTINEL).is_file():
        problems.append(f"{target} has no {WORK_SENTINEL} sentinel")
    for path in protected:
        if real == path or path.is_relative_to(real):
            problems.append(f"{target} is or contains the protected path {path}")
    return problems


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[0] != "check-delete":
        print("usage: manifest.py check-delete <work|worktree> <path>", file=sys.stderr)
        return 2
    kind, target = argv[1], pathlib.Path(argv[2])
    problems = delete_problems(
        kind, target, read_manifest(MANIFEST), protected_paths(dict(os.environ), kind)
    )
    for problem in problems:
        print(f"refusing: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
