# PR-1 SQLLogic gate: explicit waiver (legacy-only suite, legacy-off environment)

## Claim

The SQLLogic suite under `test/sql/` cannot run in this environment for reasons that predate
and are independent of PR-1, and it exercises only the legacy `gpu_processing` engine that
PR-1 does not touch. Super-Sirius-path SQL coverage for the refactor is provided by the
Catch2 integration suite instead.

## Evidence

1. Every query in the suite goes through the legacy engine, not Super Sirius:
   - `test/sql/tpch-sirius.test` wraps all 22 queries in `call gpu_processing("select ...")`
     (22 occurrences) and calls `call gpu_buffer_init("4 GB", "4 GB")` at line 151.
   - `test/sql/clickbench-sirius.test:150` and `test/sql/bugfix.test:24` likewise start with
     `gpu_buffer_init` and drive `gpu_processing`.
2. Both table functions exist only in legacy builds:
   - `gpu_processing` is registered inside `#ifdef SIRIUS_ENABLE_LEGACY`
     (`src/sirius_extension.cpp:565-573`, guard opens at `:354`).
   - `gpu_buffer_init` is registered inside `#ifdef SIRIUS_ENABLE_LEGACY`
     (`src/sirius_extension.cpp:1838-1846`).
3. This build has legacy off: `build/release/CMakeCache.txt:766` ->
   `ENABLE_LEGACY_SIRIUS:BOOL=OFF`. Running the file therefore fails at line 150/151 with
   `Catalog Error: Table Function with name gpu_buffer_init does not exist!`
   (see `pr1/test-sqllogic.log`) before any query executes -- a pre-existing environment
   condition, not a PR-1 regression. All 24 assertions before that point (DDL + COPY) pass.
4. CI does not run the SQLLogic files either: `.github/workflows/test.yml` runs the full
   Catch2 suite (`sirius_unittest --abort`, `:148-152`) and the SF1 result-validation script
   (`test/tpch_performance/benchmark_and_validate.sh 1`, `:184`); no step touches `test/sql/`.
5. PR-1 does not modify `src/legacy/` (see `git diff HEAD --name-only`), so the legacy engine
   the suite tests is bit-identical to `dev`'s.

## Super-Sirius-path SQL coverage standing in for the gate

- `test/cpp/integration/test_gpu_execution_group_join.cpp` runs real SQL through transparent
  interception (`[group_join]`: 37 cases / 826 assertions, green -- `pr1/test-group_join.log`).
- SF1000 kit A/B: all 22 TPC-H queries executed end-to-end on both binaries with fetched-row
  counts equal per query and plan/fusion/strategy/reservation parity (`pr1/plan-parity.md`).
- q13 result parity A/B at SF1 via CSV row diff (`pr1/allocset-parity.md`).

## Residual for the merge decision

If a legacy-enabled SQLLogic run is wanted anyway, it requires a rebuild with
`ENABLE_LEGACY_SIRIUS=ON` and is Kevin's call (full-suite policy); PR-1 leaves `src/legacy/`
untouched, so the expected outcome is unchanged from `dev`.
