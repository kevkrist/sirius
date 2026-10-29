# S5 domain-evidence validation

S5 implementation, source review, and the required two- and four-GPU experiment
evaluation are complete. Retain the implementation and production coverage
threshold **0.9**. The explicit **0.45** profile has a confirmed two-GPU Q21 benefit;
the four-GPU screen did not qualify for confirmation. No further experiment or
production change was indicated by the agreed protocol.

S5 addresses [issue 1824](https://github.com/sirius-db/sirius/issues/1824) in the
[dynamic-filter sequence](https://github.com/sirius-db/sirius/issues/1818). Its
immediate base is S4 commit `4d4bfdda12a88ec13b7e577b17ca40feea0316c0`
([PR 2349](https://github.com/sirius-db/sirius/pull/2349)). The measured S5 source
commit is `da7b8ab777dda089ae9087ad35cf5d7263180aea`; returned evidence was reviewed
at `0e963345aa59ec2c70615530d30a3bea6d467c51`.

The maintained implementation contract is in
[dynamic filters](../../docs/super-sirius/dynamic-filters.md#filter-selection),
[pinning](../../docs/super-sirius/scan.md), and
[configuration](../../docs/super-sirius/configuration.md).
Copied source counts and independent per-column declarations affect optional
filter usefulness only; they never become correctness-bearing uniqueness proofs.
The completed planning, handoff and intermediate review notes have been removed;
their historical versions remain in Git.

## Validation and final review

Both measured hosts used NVIDIA RTX PRO 4500 Blackwell Server Edition GPUs and
TPC-H SF100 Parquet data. The local host had two GPUs; the remote host had four.
All comparisons used the same binary and configuration on their respective host,
with 256 MB scan/concat batches, 512 MB partition/build batches, the unchanged
256 MiB per-GPU Bloom cap, and pin compression disabled. Batch-size MB is decimal.
Six independent TPC-H key declarations were audited against CPU DuckDB. Orders
has 150 million distinct keys spanning 1 through 600 million; the domain is its
row count, not its range width.

| Check | Two-GPU result | Four-GPU result |
| --- | --- | --- |
| Library, native tests and DuckDB wrapper build | Passed | Passed |
| Focused native suite | 71 cases / 8,088 assertions passed | 71 cases / 8,088 assertions passed |
| Portable runner CPU checks | 18 passed | 18 passed |
| All-22 CPU-oracle comparison | 88 GPU processes passed | 88 GPU processes passed |
| Fixed Q4/Q7/Q10/Q21 screen | Q21 0.45/0.9 qualified | No contrast qualified |
| Fresh confirmation | Q21 gain and Q4/drift controls passed | Not required by the screen |

The native suite covers source and column mismatch, preserved and unsupported
lineage, pin merge/replacement/invalidation, false hints including duplicate/NULL
data, result multiplicity, both publisher paths, allocation avoidance, an ungated
sibling, and configuration defaults/validation/SET/RESET. Every benchmark GPU
execution verifies settings, interception, topology and CPU result parity with
fallback disabled. The only approximate comparison is recognized Q1 `avg_disc`
DOUBLE column 8, with exact unique row keys and absolute OR relative tolerance
`1e-12`; other values, schemas and multiplicities are exact.

Independent standards and specification reviewers examined all 14 changed
production files and seven changed native test translation units against S4.
They refreshed the reviewer definition and documented the relevant assigned
readings read or reused. No actionable code finding remained. The parent also
reviewed the runner and native-test registration. The final source review ran no
new builds or GPU tests and did not expand the experiment conclusions.

The earlier bounded local Compute Sanitizer run passed five cases / 1,699
assertions with explicit API reporting and API memory-access checking enabled,
with zero errors. Default extended-API reporting produced 28 diagnostics whose
three signature groups and counts reproduced in the S4 Parquet GPU control:
six HOST/HOST_NUMA pool-access reports, 21 stream-context teardown reports, and
one duplicate CUB kernel-registration report. This is not a blanket sanitizer
cleanliness claim or proof that the reproduced diagnostics are harmless.

## Measured outcomes

Five fresh two-GPU process-pair blocks, each with two warmups and six measured
executions per arm, produced these process-median ratios:

| Comparison | Geometric time ratio | 95% interval | Result |
| --- | ---: | --- | --- |
| Q21, 0.45 / 0.9 | 0.948473 | [0.942788, 0.954193] | 5.15% lower latency; gain criterion met |
| Q4 no-gate control, 0.45 / 0.9 | 1.005638 | [0.993852, 1.017563] | 3% nonregression guard passed |
| Q21 same-arm control, 0.9 / 0.9 | 1.002704 | [0.996755, 1.008689] | Drift guard passed |
| Q4 same-arm control, 0.9 / 0.9 | 1.009421 | [0.989883, 1.029344] | Drift guard passed |

On four GPUs, Q21 0.45/0.9 had screen ratios **0.963951** and **0.989834**
(3.60% and 1.02% lower time). Neither reached the required 5% continuation
threshold. The 0.45/2.0 contrast also failed the both-blocks rule. The two-block
screen establishes neither a confirmed latency gain nor a 3% nonregression
guarantee. It also does not prove that a larger benefit is impossible.

The mechanism was observed on both hosts: Q21's 73,072,502 build rows cover
48.715% of its copied 150,000,000-row source domain. At 0.45 the large Bloom
filter is skipped; at 0.9 and 2.0 it is retained. Four-GPU INFO timing logs show
that 0.45 avoids a **146,145,024-byte array per GPU (139.4 MiB)** and its associated
filter work on each execution. Both smaller filters remain. This does not measure
total peak memory or transfer bytes.

The two-GPU timing result is specific to its workload and environment. These
same-S5-binary threshold comparisons do not measure total S5-versus-S4 overhead,
broad all-query performance or the integrated reference branch's overall gain.
Production remains **0.9**, and **0.45 remains experimental**.

The two-GPU summary above preserves the previously reviewed local results;
those raw local campaigns are not included in this repository. The complete
four-GPU evidence is retained in [results/](results/), with hardware/software,
binary hashes, dataset identities, commands, diagnostics and per-block timings
in [FOUR_GPU_RESULTS.md](FOUR_GPU_RESULTS.md).

## Fixed experiment protocol

The protocol was fixed before measurements. CPU DuckDB is the oracle;
`enable_dynamic_filter=false` is a separate GPU correctness control. Three
same-binary threshold arms use honest independent key declarations: production
0.9, explicit 0.45, and 2.0, which disables only the coverage gate. A no-hint 0.9
diagnostic checks declaration eligibility. DEBUG diagnostics are separate from
INFO timing processes. All valid timing samples remain, including samples with
different optional publication outcomes.

Run all-22 correctness before interpreting timings. The fixed screen covers
Q4/Q7/Q10/Q21 with two fresh-process blocks, two warmups and three measured
repetitions per arm, using seed 18235. Continue with at most one contrast whose
gate decision actually changes and whose time ratio is at most 0.95 in **both**
blocks, plus one observed no-gate control. A suspected regression may instead
receive one bounded follow-up. If none qualifies, stop without expanding the
query or threshold search.

Confirmation uses five fresh paired blocks and five same-arm control pairs per
query, with two warmups and six measured repetitions per process, seed 18236.
Equally weight the log ratios of process medians and use a 95% Student-t interval
over the five independent blocks. A gain requires a geometric ratio at most 0.95
and upper bound below 1.00. Nonregression requires an upper bound at most 1.03;
a lower bound above 1.03 confirms regression. Crossing 1.03 is inconclusive.
Same-arm drift must have point ratio in [0.97, 1.03] and an interval containing
one; a failed drift guard makes the corresponding attribution inconclusive.

The reusable runner, its CPU checks and four-GPU configuration remain under
[tools/experiments](../../tools/experiments/). The completed four-GPU build and
campaign commands are recorded in [the report](FOUR_GPU_RESULTS.md).

## Replay the returned evidence

From the repository root, verify and extract the committed archives into a new
directory. This replays saved-result validation and does not execute GPU queries.

```bash
(cd experiments/s5/results && sha256sum --check sha256sums.txt)
S5_REPLAY=$(mktemp -d /tmp/s5-replay.XXXXXX)
find experiments/s5/results -name '*.tar.xz' -print0 |
  sort -z | xargs -0 -n1 tar -xJ -C "$S5_REPLAY" -f
pixi run python tools/experiments/s5_compare.py analyze --out "$S5_REPLAY/smoke"
pixi run python tools/experiments/s5_compare.py analyze --out "$S5_REPLAY/all22"
pixi run python tools/experiments/s5_compare.py analyze --out "$S5_REPLAY/screen"
```

[index.json](results/index.json) identifies the 162 archives and 3,915 extracted
files. Per-file `.sha256` manifests use four whitespace-separated fields:
SHA-256, size in bytes, archive path, and extracted path. Archive filenames may
contain `processes/`; the extracted directories are named `runs/`.
