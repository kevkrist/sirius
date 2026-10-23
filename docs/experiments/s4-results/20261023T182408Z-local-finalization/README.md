# Local S4 finalization evidence

Two RTX PRO 4500 Blackwell GPUs, 2026-10-23. This is local correctness and bounded comparison evidence, not final four-GPU acceptance. See [the validation report](../../../../S4_LOCAL_FINALIZATION_VALIDATION.md) and [remote instructions](../../../../S4_REMOTE_EXPERIMENTS.md).

`local-evidence.tar.gz` contains 1,017 files: complete native campaign records, requests, SQL/configuration, telemetry, build/test/sanitizer logs, original and corrected artifact manifests/CMake caches, the Q1 diagnosis and regression evidence, and the exact original measured harness. It excludes datasets, compiled binaries and environments. `archive-files.json` records each member's size and SHA-256. `SHA256SUMS` covers the files alongside the archive.

| Original directory | Archive location |
| --- | --- |
| `/home/ubuntu/sirius/build/s4-final/build/s4-local-validation` | `validation/` |
| `/home/ubuntu/sirius/build/s4-prerequisite/build/s4-local-validation` | `prerequisite-checks/` |
| `/home/ubuntu/sirius/build/s4-control/build/s4-local-validation` | `control-build/` |
| Final worktree `build/s4-final-pair` and `build/s4-control-artifact` | Matching names under `artifacts/` |
| Final worktree `build/s4-final-pair-oracle` and `build/s4-control-artifact-oracle` | Matching names under `artifacts/` |

Original absolute paths in manifests and requests are preserved. The two strict-oracle comparison campaigns contain 96 samples, including warmups. Their original runner/analyzer/helper are under `validation/measured-harness/`; their hashes match both campaign manifests. The subsequent narrow Q1 oracle correction changed only test fixtures and protocol, with unchanged production-library hashes. Corrected Q1 checks passed on all three revisions. The failed original Q1 request and invalid noncontributing-root request are retained and are not counted as passing cases.

The coordinator verified every archive member and extracted the bundle into a separate directory. Reanalysis reproduced both strict-campaign reports with the preserved analyzer and the corrected Q1 report with the final analyzer; only directory locations differ. `archive-audit.json` records this verification. For example, after extraction into a directory named `extracted`:

```bash
pixi run python extracted/validation/measured-harness/analyze_s4_experiments.py \
  --results extracted/validation/prerequisite-sf100 --output prerequisite-reanalysis.json
pixi run python extracted/validation/measured-harness/analyze_s4_experiments.py \
  --results extracted/validation/control-sf100 --output control-reanalysis.json
pixi run python scripts/analyze_s4_experiments.py \
  --results extracted/validation/q1-corrected-campaign --output q1-reanalysis.json
```

Each command takes one campaign directory. Keep the original records and reports unchanged; write reanalysis into new files.
