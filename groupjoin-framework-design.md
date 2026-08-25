# GROUPJOIN Operator Framework — Design

**Status:** draft for review — design only, no code
**Scope:** generalize `DENSE_COUNT_JOIN` into a generic fused join+group-by (GROUPJOIN) framework
**Baseline:** branch `perf/dense-count-join` @ `db7bcb3c`
**Papers:** Moerkotte & Neumann, PVLDB 4(11) 2011 ("M11"); Fent & Neumann, PVLDB 14(11) 2021 ("F21")

---

## 1. Executive summary

Sirius already ships one fused join+aggregate operator: `DENSE_COUNT_JOIN` fuses
`COUNT(col|*) … GROUP BY key` over a preserved-side outer equi-join into a single two-input,
single-task operator built on direct-address histograms
(`src/op/sirius_physical_dense_count_join.cpp`, `src/cuda/dense_count_join_impl.cu`). It replaces
a ~6-pipeline join+aggregate fragment with 3 pipelines and one GPU task, and it is default-on.

This document generalizes that operator into a **GROUPJOIN framework** decomposed into five
orthogonal axes — aggregate bundle, join form, key-domain strategy, empty-group/NULL semantics,
and result consumption — such that:

1. The existing COUNT pathway lowers to **byte-identical kernels and identical pass/allocation
   structure** (§7, performance preservation argument). Templates monomorphize; nothing virtual
   touches the hot loop.
2. Two new TPC-H pathways land on the v1 mechanism: **q17** (AVG per delim partkey, true
   groupjoin over an INNER correlation join; fused with preserved-port dynamic-filter
   publication as a prerequisite, §4.9/§5.1) and **q2** (MIN per partkey, a degenerate
   single-input "DIRECT" form of the same operator — by its own gates it runs the *sparse*
   strategy inside the single fused task; the win is the fragment collapse, §5.2). A third,
   **q18** (SUM per orderkey with functionally-determined carried columns), is fully specified
   but scheduled behind the explicitly-designed sparse/remap seam (§5.4).
3. Everything else — DISTINCT aggregates, composite keys, arbitrary sparse domains, semi/anti
   membership, streamed accumulation, multi-GPU — is a named seam or a named non-goal (§8).

The theoretical foundation is deliberately **not** the papers' groupjoin operator itself but
their both-sides eager-aggregation identity (M11 Eq 13/18 with the Yan–Larson ⊗ correction):
Sirius computes `Γ_G(e1 ⋈ e2)` directly as *two independent per-key aggregations combined in a
shared dense index*, which eliminates the papers' duplicate-free-left-side precondition for
every aggregate we support (§2.3). This keeps detection statistics-free and cheap, exactly as
today.

---

## 2. Theory background (load-bearing results only)

### 2.1 Decomposable / splittable aggregates and the ⊗ correction (M11 §2, Fig. 1)

An aggregate is *decomposable* if partial aggregates merge losslessly: min/min, max/max,
count→sum, sum→sum, avg = (sum, count-of-non-NULL). An aggregation vector is *splittable* if
each component references only one join side. Both are prerequisites for computing aggregates
before/independently of a join. The **⊗ correction** (Yan–Larson, M11 §1.5 as reproduced in the
paper's definitions): when a per-key aggregate computed on one side is combined with the other
side's per-key multiplicity `c`, duplicate-*sensitive* aggregates scale — `sum(e)→sum(e)·c`,
`count(*)→c·matched` — while duplicate-*agnostic* aggregates (min, max) are unchanged, and AVG
is invariant because `c` cancels in numerator and denominator.

Today's operator already **is** an ⊗ implementation: `presence[k] × matched[k]`
(`emit_kernel`, `src/cuda/dense_count_join_impl.cu:163-189`) is exactly `count(*)⊗presence`.

### 2.2 The two rewrites and the COUNT bug (M11 Eq 9–12; F21 §2.1, Table 1)

- **Outer form** (M11 Eq 11): `Γ_G(e1 LOJ e2) ≡ groupjoin` requires condition (4):
  `F(∅) = F({⊥})` — true for min/max/sum/count(col), **false for count(*)**. SQL
  LOJ-then-GROUP-BY gives an unmatched left row count(*) = 1 (the NULL-padded row counts),
  count(col) = 0, sum/min/max = NULL. A naive fused operator that evaluates `agg(∅)` returns
  count(*) = 0 — the COUNT bug. The fix is a per-aggregate empty-group default (M11 Eq 18's
  `c2:1` LOJ default). Today's kernel implements it: `if (count_star && matched == 0) matched = 1`
  (`dense_count_join_impl.cu:180`).
- **Inner form** (M11 Eq 12): `Γ_G(e1 ⋈ e2) ≡ Π(σ_{c2>0}(groupjoin computing c2:=count(*)))`.
  No empty-group condition needed; the operator must always carry a per-key match count and
  filter `matched > 0`. This maps to a one-line change in the emit selection predicate.

### 2.3 Why Sirius does not need the duplicate-free precondition

The papers' groupjoin emits one row per left tuple, so duplicates on the left silently break the
rewrite (M11 Appendix E, counterexample 3). Sirius's operator instead computes the **group-by
result directly** when the group key *is* the join key: it materializes
`presence = Γ_key(count(*))(preserved)` and `partials = Γ_key(F¹ ∘ count(*))(counted)` into a
shared dense index and combines them with ⊗ at emit. This is M11 Eq 13/18 (both-sides eager
aggregation) specialized to `G = J1 = J2`, under which preconditions 1–2 of Eq 11/12 hold
*structurally* and left-side duplicates are handled by the ⊗ scaling itself — no FD reasoning,
no uniqueness proof, no statistics. Framework conditions that remain and must be enforced by
detection:

- aggregates reference only the counted side (splittability, M11 cond 3);
- aggregates are decomposable (COUNT/SUM/MIN/MAX/AVG per M11 Fig. 1 — all of ours);
- the group key equals the preserved-side join key (makes `G2⁺ = J2` trivial);
- empty-group defaults per §2.2 for the outer form.

FD-based generalizations — key shrinking (M11 Eq 4/6/7) and carried determined columns — are
needed only for the q18-class pathway and are explicitly deferred (§5.4).

### 2.4 Strategy economics: the eager-aggregation hazard (F21 §3, Table 3)

F21's cost model: eager right aggregation costs `|S|` and only wins when nearly every counted
tuple joins (σ_S ≈ 1, e.g. q13 with σ_S = 100%). When σ_S ≪ 1 (q17: |R| = 204 vs |S| = 6M at
SF1, σ_S = 0.10%), eager is **an order of magnitude slower** than join-filtered aggregation.
This matters directly to us: our *sparse fallback* (`cudf::groupby` partials over the whole
counted side, `sirius_physical_dense_count_join.cpp:551-651`) **is** eager right aggregation.
It is the correct backstop for q13-shaped inputs and a hazard for q17-shaped ones. §4.4's gate
policy is designed around this asymmetry.

### 2.5 Non-results we deliberately do not import

- F21's three parallel strategies target CPU cache-contention; GPU hardware atomics on dense
  arrays already give contention-free accumulation, and the dense histogram supports concurrent
  accumulation from many tasks with zero merge logic (relevant to the streaming seam, §4.8).
- F21 §4's skew-normal aggregate estimates require a stats subsystem Sirius does not have;
  DuckDB's `estimated_cardinality` plus stats-free runtime gating (today's approach) suffices
  for our gate ladder.
- M11's Q9 lesson — a rewrite applied locally can lock out a better join order — is accepted
  risk: we pattern-match DuckDB's already-optimized plan and mitigate with fail-closed gates
  and per-pathway measurement, not with join-order enumeration.

### 2.6 Related systems

Neither DuckDB, Velox, nor DataFusion ships a fused groupjoin physical operator; the production
precedents are Umbra/HyPer (F21) and System T (M11). DuckDB's closest analogue is
`PhysicalPerfectHashAggregate` (dense-domain direct addressing for group-by), whose eligibility
probe Sirius already mirrors at `src/planner/sirius_plan_aggregate.cpp:512-606`. That precedent
informs the DIRECT form (§4.3) but the join-fused forms have no OSS reference implementation —
the dense-count operator itself is the reference.

---

## 3. Dense count join today (verified anatomy)

The full anatomy is in the operator sources; this section pins the facts the framework must
preserve or generalize, with verified anchors.

**Planner.** Hook: `create_plan(LogicalAggregate&)` runs `downcast_hugeint_types` (:632), then
`try_plan_dense_count_join` (`src/planner/sirius_plan_aggregate.cpp:640`), before any generic
aggregate planning. Detection `detect_dense_count_join` (:242-360): exactly 1 group (BOUND_REF)
= preserved-side join key (:328-337); exactly 1 aggregate that passes the paranoid
builtin-COUNT catalog identity check `exact_builtin_count_id` (:195-240); no
DISTINCT/FILTER/ORDER BY (:261); child is `LOGICAL_COMPARISON_JOIN` LEFT/RIGHT with one
COMPARE_EQUAL condition on BOUND_REFs, no residual predicate (:268-303); keys both INTEGER or
both BIGINT (:313-317); projection maps and output types re-validated (:279-297 via
`validate_join_projection_layout` :143-158 and `resolve_join_output_column` :160-173); both
children linear GET/FILTER/PROJECTION scan chains (`is_linear_scan_chain` :176-191); COUNT(col)
argument resolves to the counted side (:348-358). **No statistics are consulted.** Construction
(:364-418) captures child cardinalities before `create_plan` drains them and emits the operator
with `op_params.dense_count_join_max_bytes`.

**Operator.** `sirius_physical_dense_count_join` is source+sink
(`src/include/op/sirius_physical_dense_count_join.hpp:80-81`) with named FULL-barrier ports
"preserved"/"counted" (hpp:60-61). Port/barrier policy is per-operator virtuals
`input_port_for`/`input_barrier_for` (`sirius_physical_dense_count_join.cpp:287-304`) consumed
generically by the converter (`src/pipeline/sirius_pipeline_converter.cpp:211-218`).
`build_pipelines` (:306-328) collapses the fragment to 3 pipelines; `get_next_task_hint`
(:330-343) waits for both producers and fires exactly one task; `get_next_task_input_data`
(:345-363) drains both ports into a side-tagged `dense_count_join_input`. Excluded from
terminal-sink merge fusion (`src/planner/sirius_physical_plan_generator.cpp:858`). Output schema
hard-coded `[key, BIGINT]` (ctor :272-276).

**Execution.** `execute` (:411-661): per-batch column harvesting with checked row accounting
(:460-488); `dense_count_global_minmax` (`dense_count_join_impl.cu:395-443`) — per-batch
`cudf::minmax` merged on-device by 1-thread kernels, **one unconditional stream sync** for the
extrema readback (:441). Dense gate `dense_ok` (:515-525): layout-valid range; range ≤
budget/(2·slot_bytes) with 2 GiB default; range ≤ 8× non-null preserved rows; range ≤ 2× total
rows; histogram bytes ≤ 4× input logical bytes. Slots are uint32 unless a side ≥ 2^32 rows
(:510-512). Dense path: two zeroed histograms (`dense_count_state` ctor, impl.cu:445-469);
grid-stride `accumulate_kernel` (impl.cu:126-155; 256-thread blocks, ≤4096 blocks, :51-59) —
preserved side unchecked, counted side bounds-checked, NULL keys/args skipped via masks; emit =
`thrust::count_if`/`copy_if` over the range with `rmm::exec_policy(stream, mr)` (impl.cu:331)
plus `emit_kernel` writing `key = min + slot`, `value = presence × matched` with the count(*)
empty-group fix (:163-189); overflow validated on-device only when the host coarse bound
`count_product_needs_validation` (.cpp:107-116) is inconclusive (impl.cu:344-380). NULL
preserved keys become one appended NULL-group row (impl.cu:263-313). Sparse fallback:
per-batch `cudf::groupby` COUNT partials widened to INT64 (.cpp:119-139), balanced pairwise
merges (:169-198), `cudf::left_join` with `null_equality::UNEQUAL` of the distinct-key tables,
`presence × matched` (:551-651).

**Memory/config.** `no_history_peak_memory_estimate` (:365-409) with saturating arithmetic;
OOM retry-reservation floor (`src/include/pipeline/gpu_pipeline_task.hpp:93-130`). Knobs:
`enable_dense_count_join` default true (`src/include/sirius_config.hpp:101,186`);
`dense_count_join_max_bytes` engine-owned 2 GiB (hpp:103,189) — the YAML key throws
(`src/sirius_config.cpp:299-303`), surviving only as an internal test-hook SQL setting.
Compressed-materialization arms: `src/planner/sirius_plan_narrowing_policy.cpp:439-457` (keys
`boundary_restore`, COUNT arg stays narrow, no forwarded carrier) and
`src/planner/sirius_plan_compressed_schema.cpp:392-410` (native keys, no output sidecar).

**Perf invariants** (must survive the refactor; see §7): single task, one unconditional sync,
one histogram pass per batch per side + 2 range passes + 1 emit, the unchecked-preserved /
checked-counted asymmetry, 32-bit slot preference, the four dense gates with their constants,
256/≤4096 launch geometry, the exact allocation set, balanced sparse merge, cheap-first
stats-free gating, estimate↔allocation agreement, `min + slot` key emission with no gather.

---

## 4. Framework design

### 4.1 Axes and roles

```mermaid
graph TD
  subgraph "Planner (policy)"
    DET[detection ladder<br/>sirius_plan_aggregate.cpp] -->|emits| SPEC[group_join_spec<br/>form + key cols + slot specs + budget]
  end
  subgraph "Operator (wiring)"
    SPEC --> OP[sirius_physical_group_join<br/>ports, barriers, pipelines, task hint,<br/>peak-memory estimate]
  end
  subgraph "Execution (mechanism)"
    OP --> SEL[runtime strategy selection<br/>extrema -> dense gates per form]
    SEL --> DENSE[dense_direct state<br/>presence + matched + payload arrays]
    SEL --> SPARSE[sparse exact fallback<br/>cudf groupby partials + join + ⊗]
    DENSE --> EMIT[emit: select + finalize + ⊗<br/>form-specific predicate]
  end
  subgraph "Compile-time (bundles)"
    B0[COUNT bundle == today's kernels]
    B1[SUM bundle]
    B2[MIN/MAX bundle]
    B3[AVG bundle]
  end
  B0 & B1 & B2 & B3 -.monomorphized kernels.-> DENSE
```

Policy (which shapes fuse, which budgets apply, which semantics hold) lives entirely in the
planner spec and in per-form gate tables; mechanism (histograms, atomics, selection, emission)
is shared and parameterized by compile-time bundles. Nothing on the device hot path dispatches
dynamically.

### 4.2 Axis A — aggregate specification

**Planner-side spec (runtime values, policy):**

```cpp
namespace sirius::op::groupjoin {

enum class join_form : uint8_t {
  OUTER_PRESERVING,  // today's LEFT/RIGHT: emit every present group (COUNT bug semantics apply)
  INNER,             // M11 Eq 12: emit iff presence > 0 && matched > 0
  DIRECT             // single input, plain GROUP BY semantics (NULL key is a group)
};

enum class agg_op : uint8_t { COUNT_STAR, COUNT_VALID, SUM, MIN, MAX, AVG };

struct slot_spec {
  agg_op op;
  std::optional<std::size_t> arg_idx;   // counted-side column; nullopt for COUNT(*)
  sirius::logical_type output_type;     // declared result type (drives finalize + widening)
};

struct group_join_spec {
  join_form form;
  std::size_t preserved_key_idx;        // unused for DIRECT
  std::size_t counted_key_idx;
  duckdb::vector<slot_spec> slots;      // v1 detection emits exactly one; mechanism takes N
  uint64_t max_state_bytes;             // engine-owned, per-form (§4.7)
};

}  // namespace sirius::op::groupjoin
```

**Kernel-side policy (compile time, mechanism).** Each `agg_op` maps to a *slot policy* — the
device-side accumulator — constrained by a concept so bundles are checked at compile time and
inheritance never reaches the loop:

```cpp
template <class P>
concept slot_policy =
  std::is_trivially_copyable_v<typename P::slot_type> &&
  requires(typename P::slot_type* s, typename P::arg_type v) {
    { P::init_fill } -> std::convertible_to<slot_init>;   // ZERO_MEMSET or VALUE_FILL
    { P::update(s, v) } -> std::same_as<void>;            // __device__ atomic RMW
  };

// Exemplars (device code in group_join_impl.cu):
template <class CountT> struct count_slot {          // == today's histogram_add (impl.cu:118-124)
  using slot_type = CountT; using arg_type = no_arg;
  static constexpr slot_init init_fill = slot_init::ZERO_MEMSET;
  __device__ static void update(slot_type* s, no_arg) { histogram_add(s); }
};
struct sum_slot_i64 {                                 // SUM/AVG numerator (unscaled decimals too)
  using slot_type = int64_t; using arg_type = int64_t;
  static constexpr slot_init init_fill = slot_init::ZERO_MEMSET;
  __device__ static void update(slot_type* s, int64_t v) { atomicAdd(/*ull*/ s, v); }
};
struct min_slot_i64 {                                 // init = INT64_MAX via thrust::fill
  using slot_type = int64_t; using arg_type = int64_t;
  static constexpr slot_init init_fill = slot_init::VALUE_FILL;
  __device__ static void update(slot_type* s, int64_t v) { atomicMin(s, v); }
};
```

**Bundles.** A bundle is the fixed tuple of arrays one accumulate pass updates. Whitelisted
instantiations (explicit-instantiation list in the `.cu`; no combinatorial template explosion):

| bundle | arrays (all length `range`)                                | per-row device work (counted pass) |
|---|---|---|
| COUNT  | `matched:CountT`                                           | 1 atomicAdd — **today's kernel** |
| SUM    | `matched:CountT`, `sum:int64`                              | 2 atomics |
| MIN/MAX| `matched:CountT`, `extreme:int64`                          | 1 atomicAdd + 1 atomicMin/Max |
| AVG    | `matched:CountT`, `sum:int64`                              | 2 atomics |

plus `presence:CountT` for the two-input forms (preserved pass — identical kernel for every
bundle, i.e. exactly today's `accumulate_preserved`, impl.cu:471-482). `matched` is mandatory in
every bundle: it is M11 Eq 12's intrinsic `c2` (INNER emit filter), the ⊗ multiplier input, and
COUNT's own value. When the aggregate argument is nullable those are **different quantities**
(today's kernel skips NULL-arg rows via the value mask, impl.cu:145-147, so a single `matched`
array would count valid-arg rows, not key matches). v1 does not carry two counts; it makes them
provably equal:

**Argument-validity gate (dense precondition, value bundles and COUNT(col) on INNER/DIRECT):**
the dense strategy is selected only if the argument column reports `null_count() == 0` across
every harvested counted batch — known before state allocation in the one-shot schedule
(`execute` harvests all batch views first, .cpp:460-488) at metadata cost. Under the gate,
per-key valid-arg count ≡ key-match count ≡ `c2`, so one `matched` array is simultaneously the
INNER emit filter, the ⊗ input, COUNT(col)'s value, and AVG's divisor — and AVG needs no
separate `valid_cnt` array at all. Any NULL argument (organic, or padding from an outer join
inside a DIRECT child, §4.5) routes the whole task to the **sparse mask-preserving strategy**
(§4.4), which is exact for nullable arguments. A separate-`c2`-plus-valid-count dense state is a
named seam, unbuilt until a pathway needs it. All named TPC-H pathway columns (l_quantity,
ps_supplycost) are NOT NULL, so the gate never fires on them.

**Init, finalize, overflow — per-op policy table (the rest of axis A):**

| op | slot init | emit finalize | empty-group value (OUTER) | overflow policy |
|---|---|---|---|---|
| COUNT(col) | memset 0 | `presence × matched` (valid ≡ matched under the argument-validity gate) | 0 | today's: host coarse bound (.cpp:107-116) → device flag (impl.cu:182-186) |
| COUNT(*) | memset 0 | `presence × max(matched,1)` | presence (≥1) | same |
| SUM | memset 0 | `presence × sum`, cast to declared type (DECIMAL64→128 as `gpu_aggregate_impl.cpp:420-435` does) | NULL (v1: form excluded, §4.5) | host bound `counted_rows × max(|vmin|,|vmax|)` from value extrema folded into the single sync; inconclusive ⇒ decline dense → sparse (parity with generic GPU int64 accumulation) |
| MIN/MAX | fill sentinel | raw slot; validity = `matched>0` | NULL (v1: excluded) | none needed |
| AVG | memset 0 | `DIV(sum, matched)` (divisor ≡ valid count under the gate) — reuse the finalize construction of `sirius_physical_grouped_aggregate_merge.cpp:259-284` (DECIMAL fixed-point vs FLOAT64) | NULL (v1: excluded) | as SUM |

AVG needs no ⊗ multiply — presence cancels (§2.1). SUM/COUNT do (`presence × …`), MIN/MAX are
duplicate-agnostic and take the raw slot. Finalization for AVG happens on the emitted
`num_groups`-sized columns via one `cudf::binary_operation` — reuse over re-implementation; at
group cardinalities (≤ a few hundred K for every pathway) this is noise.

Argument types v1: INT32, INT64, DECIMAL32, DECIMAL64 (accumulated as unscaled int64 —
`sum_slot_i64` — with declared-type cast at emit). No FLOAT v1 (no pathway; atomicAdd(double)
is a one-line seam).

### 4.3 Axis B — join-form subset

Supported forms and why:

| form | semantics | needed by | emit selection predicate |
|---|---|---|---|
| `OUTER_PRESERVING` | LEFT/RIGHT outer, preserved = group side; SQL LOJ+GROUP BY semantics incl. the COUNT bug fix | q13 (existing) | `presence > 0` (today's `presence_positive`, impl.cu:157-161) |
| `INNER` | M11 Eq 12; the fused histogram **is** the join | q17 (and q20/q18 later) | `presence > 0 && matched > 0` |
| `DIRECT` | no join; single counted input; plain GROUP BY over an arbitrary subtree — the degenerate case where the presence side is provably redundant | q2 | `matched > 0`; NULL keys form a real group (extra slot, §4.5) |

Excluded, with one-line justifications:

- **SINGLE** — Sirius hash join already rejects it (`docs/super-sirius/operators.md:249-262`);
  no pathway.
- **SEMI/ANTI/MARK** — membership, not aggregation. The q4/q22 dense-membership bitmap is a
  worthwhile *sibling* operator (19 MB custkey bitmap at SF1000) hooking at the delim join, not
  at the aggregate; it shares the domain machinery but nothing of the aggregate axes. Out of
  scope here; the domain-strategy interface (§4.4) is written so it can be reused.
- **FULL OUTER** — no pathway; both-sides-preserved semantics would need a second NULL-group
  mechanism for unmatched counted keys.
- **IS NOT DISTINCT FROM keys** — every identified correlation join under an aggregate uses
  plain `=` (q17: `l_partkey = p_partkey`); INDF appears only at the delim-join level above,
  which we do not fuse. Seam: an `null_safe_keys` flag in the spec would change only the NULL
  handling rows of §4.5.

DIRECT is included rather than left to the generic aggregate because (a) it falls out of the
axes for free — it is the two-input mechanism with the presence array and its pass deleted;
(b) it is the *honest* formulation of q2, where a true groupjoin at either join edge is invalid
or a pessimization (§5.3); (c) gating it to join-rooted single-key shapes keeps it from
re-litigating general aggregation planning (the perfect-hash probe territory,
`sirius_plan_aggregate.cpp:512-606`).

### 4.4 Axis C — key-domain strategy

**v1 strategy: `dense_direct`** — today's mechanism unchanged: runtime extrema
(`dense_count_global_minmax`), `offset = (u64)key − (u64)min` (impl.cu:149-152), direct-address
arrays, `min + slot` key reconstruction at emit (no gather). Generalizations:

- `combined_slot_bytes` generalizes from `2 × slot` to `Σ` over the form's array set (presence?
  + matched + payloads). All budget math keys off this sum.
- **Per-array count widths (new forms only).** Today's rule promotes *both* count arrays to
  64-bit when *either* side ≥ 2^32 rows (.cpp:510-512) — correct because a per-key count is
  bounded only by its side's row count, but wasteful when the sides are asymmetric. New forms
  derive each width from its own bound: `presence` is u32 iff preserved rows < 2^32, `matched`
  is u32 iff counted rows < 2^32; payload arrays (`sum`, `extreme`) are always 64-bit. Same
  proof shape as today (per-key count ≤ side rows), no saturation flag needed. The COUNT bundle
  (P0) keeps the joint promotion rule **verbatim** as part of the R1 guarantee (§7.3). Concrete
  consequence: q17 at SF1000 unfiltered (counted lineitem ~6 B rows ≥ 2^32, preserved ~540 K)
  gives presence:u32 + matched:u64 + sum:i64 = 20 B/slot, not the joint-rule 24 B/slot that
  would bust the budget; when the preserved-port membership filter (§4.9) collapses the counted
  side below 2^32 rows, the runtime derivation yields 16 B/slot automatically.
- Value extrema (needed by SUM/AVG overflow bounds) merge into the same device extrema array
  (2 slots → 2 + 2·num_sum_slots) and are read back **in the same single memcpy/sync**
  (impl.cu:436-441). The count bundle keeps a 2-slot array and identical kernels.

**Gate policy is per-form** (the load-bearing policy/mechanism split):

| gate (today's letter, .cpp:515-525) | COUNT / OUTER_PRESERVING | INNER (delim-provenance) / DIRECT value forms |
|---|---|---|
| (a) layout valid (range ≠ 0, ≤ SIZE_MAX/slots, ≤ INT64_MAX) | unchanged | unchanged |
| (b) state ≤ budget | `dense_count_join_max_bytes` = 2 GiB, unchanged | `group_join_max_state_bytes`, engine-owned `min(16 GiB, device memory / 16)` (§4.7) |
| (c) range ≤ 8 × non-null preserved rows | unchanged | **dropped** |
| (d) range ≤ 2 × total rows | unchanged | unchanged |
| (e) state bytes ≤ 4 × input logical bytes | unchanged | unchanged |

Rationale for dropping (c) on the new forms: gate (c) prefers sparse when the histogram is much
larger than the live key set. For COUNT shapes that is right — the sparse path aggregates a
counted side whose keys are mostly live (q13: σ_S = 100%). For delim-sourced INNER shapes the
preserved side is *intentionally tiny* (q17: ~540 K live keys in a 200 M domain at SF1000) while
the counted side is enormous (lineitem 6 B rows, σ_S ≈ 0.27%); falling to sparse there means
eager-aggregating 6 B rows for 540 K groups — F21's measured 10× pathology (§2.4). The honest
cost accounting of the dense alternative has **two terms**:

- *State fill + emit scan:* ~2× state bytes of streaming traffic (~8 GB for a 200 M × 20 B
  bundle) — single-digit milliseconds at HBM bandwidth; minor.
- *Scattered accumulation (dominant when counted rows are billions):* N_counted × 2–3 atomics
  over a state region far exceeding L2 (~126 MB vs 3–4 GB ⇒ few-percent hit rate), i.e.
  approximately one DRAM read-modify-write per atomic — for 6 B rows, hundreds of GB of
  effective traffic and atomic-throughput-limited: **~100 ms-class on GB300, not single-digit
  ms**. This is the term gate-(c)'s drop is actually betting on.

The bet is still correct *for the decision gate (c) governs* — dense vs sparse over the same
inputs: the sparse alternative hashes, materializes, and merges the same N_counted rows
(strictly more traffic per row than 2–3 atomics) and adds the 16× memory profile, so dense
dominates sparse at every σ for these shapes. What the atomic term does change is the
**fused-vs-unfused** economics: the hash-join probe the fusion deletes walks a ~540 K-entry
(~8–16 MB) fully-L2-resident table, and with the baseline's membership dynamic filter firing
(§4.9) the deleted downstream fragment operates on ~16 M rows, not 6 B. §5.1's estimate is
therefore explicitly conditional on this asymmetry, and a **microbenchmark — 6 B random
atomicAdds over a 3.2–4 GB array on GB300 — is a PR-3 merge prerequisite** (§9, §10). Slot
budget remains bounded by gates (b), (d), (e); the dead-slot waste is the correct trade.

**Sparse-exact fallback (mechanism kept, per-bundle, mask-preserving):** per-batch
`cudf::groupby` SUM/MIN/MAX/COUNT partials (the value analogue of `sparse_partial_count`,
.cpp:119-139) + balanced pairwise merge (:169-198) + distinct-key join + ⊗ combine. For the new
forms it is additionally the **NULL-argument-exact** path (the target of the argument-validity
gate, §4.2): cudf aggregations exclude NULL arguments and produce NULL for all-NULL groups, and
the emitted aggregate columns carry their validity masks through merge and ⊗ (NULL ⊗ c = NULL).
Two spec deltas beyond the value analogues: (i) INNER realizes M11's `c2 > 0` emit filter
**structurally**: a key appears in the counted distinct-key partials iff at least one key-match
row exists (cudf groupby emits a group for every non-NULL key with rows, even when every
argument is NULL), so the distinct-key **inner** join of the two sides *is* the σ_{c2>0} — no
COUNT(*) column is materialized. This is sound because `c2`'s value (as opposed to its
positivity) is only ever emitted for COUNT(*) itself, which takes no argument and therefore
never coexists with a nullable argument; COUNT(arg) (the value / AVG divisor) is still computed
per key, and valid-arg count ≠ key-match count remains true but immaterial;
(ii) DIRECT includes NULL keys as a real group (groupby NULL-inclusive keys), whereas the join
forms keep today's `null_equality::UNEQUAL` distinct-key join. This is the correctness
backstop — once planned, the operator must never be wrong, only slower — same stance as today
(runtime bail is dense→sparse only). Its eager-cost hazard is mitigated by the gate policy
above making it unreachable for pathological *large-counted-side* inputs on the new forms; it
remains reachable, and benign, where the counted side is small — a filtered q17 (§5.1), q2
(§5.2), nullable arguments — and for adversarial extrema (e.g. one key = 1, one key = 2^62),
where no in-operator strategy is fast.

**Named seam (not built in v1): `preserved_remap`.** For domains where `dense_direct` is
infeasible even with relaxed budgets (q18: o_orderkey spans ~6e9 ⇒ ≥ 48 GB dense), build a
dictionary over the preserved keys — `cudf::distinct_hash_join` on the preserved key column,
uniqueness supplied by DELIM provenance or `prove_unique_columns`
(`src/planner/sirius_plan_comparison_join.cpp:63-199`, used at :448, `unique_build_keys` :744) —
then per counted batch probe → (build_row, probe_row) index pairs → scatter-accumulate into
arrays indexed by build row. Slot id = build row makes carried-column emission a plain
`cudf::gather`. Interface impact is confined to: a `domain_strategy` enum in the runtime
selection, a second `state` implementation, and a dictionary-size cap (hard-learned precedent:
the runtime distinct probe caps at 128 M rows, `k_max_distinct_probe_rows`,
`src/op/sirius_physical_hash_join.cpp:1534`; uniqueness rules `operators.md:270-280`). Nothing in the bundle, form, or emit
axes changes. This seam is specified so §5.4 can be scheduled without redesign, and it is the
reuse point for the future membership sibling.

### 4.5 Axis D — empty-group and NULL semantics

Normative table (SQL semantics; M11 Eq 9–12, F21 Table 1):

| situation | OUTER_PRESERVING | INNER | DIRECT |
|---|---|---|---|
| group with preserved rows, no counted match | COUNT(col)=0; COUNT(*)=presence (padded rows count); SUM/MIN/MAX/AVG=**NULL** | row dropped (σ_{c2>0}) | n/a (group exists iff rows exist) |
| NULL preserved key(s) | one NULL group; COUNT(*)=n_null, COUNT(col)=0 (today: impl.cu:263-313, .cpp:640-650); value aggs NULL | dropped (`=` never matches; NULL keys skipped in presence pass — today's mask skip) | n/a |
| NULL counted key | never matches; skipped (mask skip / bounds check) | same | **forms the NULL group** — GROUP BY groups NULLs (M11 §1.8, Γ uses ≐). Mechanism: one extra slot at index `range` receiving NULL-key rows (kernel: `offset = key_is_null ? range : key − min`), zero extra passes |
| NULL aggregate argument | skipped for COUNT(col)/SUM/MIN/MAX/AVG-numerator (value mask, today's mechanism impl.cu:144-147); COUNT(*) counts the row | same | same |
| group with key matches, **all** aggregate arguments NULL | COUNT(col)=0; COUNT(*)=matched; value aggs **NULL** (v1: form excluded) | row **kept** (c2 = key matches > 0): COUNT(col)=0, COUNT(*)=matched, SUM/MIN/MAX/AVG=**NULL** | same as INNER (group exists because rows exist) |

The last row is the F(∅) ≠ F({⊥}) case resurfacing *inside* a group, and it is why NULL-bearing
arguments never reach the dense state: the argument-validity gate (§4.2) routes any task whose
argument column has `null_count() > 0` to the sparse mask-preserving path (§4.4), which produces
exactly this row's values including the output NULL masks and the c2-vs-valid-count split. Note
this covers **padding NULLs as well as organic ones** — e.g. a DIRECT child containing an outer
join (`SELECT c_custkey, sum(o_totalprice) FROM customer LEFT JOIN orders … GROUP BY c_custkey`,
which rung P2 matches): the padded rows' NULL arguments are visible in the harvested batches'
`null_count()`, the gate fires, and the sparse path emits SUM = NULL for order-less customers —
the dense path, whose memset-0/sentinel slots and mask-less emit would silently return 0 /
sentinel, is never entered for such inputs. DIRECT's child may therefore stay opaque to join
types (§5.2) without re-admitting value-over-outer semantics.

**The COUNT-bug cell** is the first row: count(*) = presence (≥ 1) vs count(col) = 0 vs value
aggregates = NULL. Today's operator implements the two COUNT columns of that row exactly. The
value-aggregate cell requires emitting a **null mask on the aggregate output** — a mechanism the
dense emit does not have (output columns are UNALLOCATED-mask fixed-width, impl.cu:340-343, with
only the key gaining a mask for the NULL group; the sparse path's cudf outputs do carry masks,
which the new forms' output schema must admit). **v1 sidesteps the dense case soundly: detection
refuses value bundles on OUTER_PRESERVING** (no pathway needs the combination — q13 is COUNT;
q17/q2 are INNER/DIRECT where empty groups are dropped). The seam is named: value-over-outer =
add a `matched > 0`-driven validity mask at emit; until built, those shapes take the generic
path.

### 4.6 Axis E — result consumption

Output schema: `[group key, finalized aggregate columns...]` — join-output columns, exactly what
the logical aggregate declares. All v1 pathways' consumers need nothing else:

- q13: `[c_custkey, count]` (existing).
- q17: `PROJECTION[0.2*avg, p_partkey]` sits above (plan: `AGGREGATE[p_partkey; avg]` →
  projection); the delim join above handles the rejoin of the scalar-per-key result. No
  operator-level scalar mode is needed — DuckDB's unnesting already turned the scalar subquery
  into a per-key column, which is the point of the delim machinery.
- q2: `PROJECTION[min, p_partkey]` likewise.

**Carried payload columns** (non-key columns functionally determined by the key — q18's
`c_name, c_custkey, o_orderdate, o_totalprice`) are part of the spec surface
(`carried_cols: vector<size_t>` on the preserved side, emitted by gather at slot→row resolution)
but **unimplemented in v1**: they require `preserved_remap` (slot = build row) to be cheap, and
only q18/q20 need them. Declared seam, §5.4.

### 4.7 Planner detection layer

One entry point replaces today's: `try_plan_group_join(LogicalAggregate&)` at the same hook
(`sirius_plan_aggregate.cpp:640`), structured as a **fail-closed ladder** evaluated cheapest
first, preserving today's invariant that detection consults no statistics and does catalog work
only after shape screens pass:

```
0. config gates: enable_dense_count_join (pathway P0) / enable_group_join (P1, P2)
1. P0 count detection — today's detect_dense_count_join checks, verbatim, in today's order
   → count spec (OUTER_PRESERVING, COUNT bundle)
2. P1 inner-groupjoin detection (q17 shape, §5.2)
3. P2 direct detection (q2 shape, §5.3)
n. nullptr → generic join + aggregate planning (unchanged)
```

Shared detection infrastructure (extracted, not changed): projection-map layout validation
(:143-173), the catalog identity check generalized from COUNT to a table of exact builtin
signatures for `avg/sum/min/max` (same canonicalization pattern as `exact_builtin_count_id`
:195-240 — canonical name, arity, return type, no varargs, null-handling class, `bind_info ==
nullptr`, system-catalog identity, fail-closed on ambiguity), `is_linear_scan_chain` (:176-191)
extended with a variant that also accepts a childless `LOGICAL_DELIM_GET` leaf for the preserved
side (planned by `create_plan(LogicalDelimGet&)` into a `DELIM_SCAN` column-data scan,
`src/planner/sirius_plan_delim_get.cpp:24-36`).

*Realized (PR-3):* the value-aggregate identity check cannot be a static signature table —
DuckDB's binder rewrites the `min/max/avg/sum` catalog entries per argument type (`BindMinMax`
specializes the ANY entry; DECIMAL `avg`/`sum` retarget to the matching integer kernels, `avg`
attaching a scale bind datum) — so `exact_builtin_value_aggregate_id` authenticates by
reproducing the binder's work: re-bind the system catalog's own entry over the same argument
type and require the query's bound function (callbacks, signature, flags, and bind data) to
equal the reproduction exactly. `sum_no_overflow` (the optimizer's stats rewrite of `sum`) is
compared against its catalog member directly and admitted as SUM. Any mismatch, ambiguity, or
throw is a miss.

Every pathway emits a `group_join_spec` (§4.2); construction mirrors today's
(:364-418): capture child cardinalities, plan children, attach `[preserved, counted]` (or
`[counted]` for DIRECT).

**Config knobs (existing naming conventions):**

| knob | kind | default | gates |
|---|---|---|---|
| `operator_params.enable_dense_count_join` | YAML bool (existing, `sirius_config.cpp:298`) | true | P0 count pathway — semantics unchanged |
| `operator_params.enable_group_join` | YAML bool (new) | **false** until Phase-3 measurement gate, then true (flipped in PR-3 after its §9 gates passed) | P1 + P2 value pathways |
| `group_join_max_state_bytes` | engine-owned, internal test hook only (pattern: `sirius_config.cpp:299-303` + `SIRIUS_ENABLE_TEST_OPTIONS` SQL setting) | `min(16 GiB, smallest visible device's memory / 16)`, falling back to the 2 GiB count-form budget when no device is visible — device-fraction-capped so small-HBM devices decline dense instead of planning unreservable state; derived from the corrected per-array widths (§4.4): q17-class 200 M-slot AVG state = 3.2–4.0 GB (SF1000), 6.4–8.0 GB (SF2000), 9.6–12 GB (SF3000), all ≤ 16 GiB on GB300-class devices | value-form gate (b) |
| `group_join_counted_bytes_gate` | engine-owned, internal test hook only (same pattern) | smallest visible device's memory / 24; 0 (decline everything) when no device is visible | value-form plan-time counted-side gate (§4.8) |
| `dense_count_join_max_bytes` | engine-owned (existing) | 2 GiB | count-form gate (b), untouched |

One new public knob total; no per-pathway knob sprawl. With `enable_group_join = false` the
planner's decisions are bit-identical to today (P1/P2 ladder rungs are behind the bool).

### 4.8 Execution layer

**Operator.** `sirius_physical_group_join` (enum `GROUP_JOIN`, replacing `DENSE_COUNT_JOIN` in
`src/include/op/sirius_physical_operator_type.hpp` and all integration arms in the same PR).
Source+sink; ports "preserved"/"counted" (DIRECT registers only "counted"); FULL barriers. The
`dense_count_join_input` side-tagging generalizes trivially (DIRECT has one side). Pipeline
construction and port wiring are **per-provenance, not "identical to today"** — today's bodies
survive only for provenance class (i):

**Wiring by input provenance.** Three classes, dispatched on the child's shape:

1. *Linear scan chain* (both P0 children; P1's counted child): today's `build_pipelines`
   verbatim — each child becomes the sink of a fresh child meta pipeline
   (`sirius_physical_dense_count_join.cpp:306-328`), and `input_port_for` maps direct children
   to their ports (:287-298). Unchanged.
2. *Routing-only `DELIM_SCAN` preserved child* (P1). Today's body is **wrong for this class** on
   two counts, both fixed here. (a) `DELIM_SCAN` never produces data — its `build_pipelines`
   registers a scheduling dependency on the delim distinct chain and appends nothing
   (`sirius_physical_column_data_scan.cpp:56-80`, with a `D_ASSERT` on the
   `delim_join_dependencies` entry); wrapping it as the sink of a fresh producer pipeline would
   skip that mandatory protocol and leave a sourceless dead pipeline. The fused op's
   `build_pipelines` therefore branches: a child of type `DELIM_SCAN` gets
   `child.build_pipelines(host_current, sink_meta)` invoked in place — dependency registered on
   the fused op's sink pipeline, no producer pipeline created. (b) The preserved data arrives
   via the converter's owning-delim retarget: the distinct chain's output pipeline is wired to
   the delim scan's *parent* with the **distinct-chain root as producer**
   (`sirius_pipeline_converter.cpp:272-286`) — a producer that is not a direct child, on which
   today's `input_port_for` throws (:296-297; today's unfused plans survive only because the
   scan's parent is a PARTITION whose base-class `input_port_for` accepts any producer,
   `sirius_physical_operator.cpp:151-155`). New arm, mirroring hash join's CONCAT precedent
   (`sirius_physical_hash_join.cpp:203-210`): a producer whose `owning_delim_join()` is non-null
   and whose delim join's `delim_scans` contains `children[0]` maps to the "preserved" port;
   any other non-child producer still throws (fail-closed). `input_barrier_for` stays FULL.
   `get_next_task_hint` runs today's *code* unchanged, with restated semantics: the preserved
   port's `src_pipeline` is the distinct chain's MERGE_GROUP_BY pipeline, and the hint waits on
   its `is_pipeline_finished()` exactly as for any producer. A conversion test for the
   fused-under-delim shape is part of PR-3's gate (§9, §10).
3. *Opaque single child* (P2 DIRECT). Today's `build_pipelines` throws on
   `children.size() != 2` (:310-313) and its `build_child_side` rejects non-unary subtree roots
   (:316-324) — both unusable for a COMPARISON_JOIN-rooted child. DIRECT takes the standard
   single-child sink pattern (base-class shape, `sirius_physical_operator.cpp:167-181`):
   `create_child_meta_pipeline(current, *this)` + `child_meta.build(*children[0])`; one
   "counted" port wired via the uniform tree-parent lookup; hint degenerates to "counted
   producer finished".

**Runtime strategy selection** (host, in `execute`): unchanged shape — harvest batches →
extrema (single sync) → per-form gate table (§4.4) → `dense_direct` state or sparse fallback.
The state object generalizes `dense_count_state` (impl.cu:445-536):

```cpp
template <class KeyT, class Bundle>          // Bundle: compile-time list of slot policies
class group_join_state {
  // presence (two-input forms), matched, payload arrays; init per slot_init policy
  void accumulate_preserved(column_view keys, stream);                    // == today's, all bundles
  void accumulate_counted(column_view keys, arg_views, masks, stream);    // one fused kernel/batch
  std::unique_ptr<cudf::table> emit(emit_policy, stream, mr) const;       // select + ⊗ + finalize
};
```

`accumulate_counted` is one grid-stride kernel per batch updating every bundle array (mask
checks once per row, then 1–3 atomics). Emit: `count_if`/`copy_if` with the form's predicate
functor, then one `emit_kernel<KeyT, Bundle>` writing keys and raw/⊗-scaled values, then
host-side finalize (AVG DIV, decimal casts) on the group-sized columns.

*Realized shape (PR-2):* the count bundle keeps the `group_join_state` class verbatim (R1); the
value forms are two per-form dense drivers — `group_join_dense_inner<KeyT, PresenceT, MatchedT,
ArgT>` and `group_join_dense_direct<KeyT, MatchedT, ArgT>` — sharing one counted-pass launcher
(`accumulate_counted_form`, parameterized by `null_slot`/`bounds_check`, which is how INNER's
skip-NULL-keys and DIRECT's NULL-group slot are the same kernel) and the slot-policy structs. The
aggregate op is a host-side `dense_value_op` switch *inside* the monomorphized driver rather than
a compile-time `Bundle` parameter: this keeps the explicit-instantiation whitelist at
16 INNER + 8 DIRECT type combinations instead of multiplying by op, while every device kernel
remains statically dispatched (`accumulate_value_kernel<…, Payload>`), preserving §4.1's
no-dynamic-dispatch-on-device invariant. The drivers are internally phased
(allocate → accumulate-per-batch → emit), so PR-5's BUILD_STREAM split into a state object is a
mechanical refactor, not a redesign.

**Memory estimate.** `no_history_peak_memory_estimate` generalizes :365-409 formula-for-formula:
`state_bytes = min(budget, 4 × input_bytes)` with the bundle's Σ-slot width; dense peak = floor
+ state + selected + outputs (+ finalize temporaries for AVG: the divisor column plus the
FLOAT64 branch's cast temporaries — charged as 3 group-sized INT64 columns plus the
declared-type output) +
mask + state-again (CUB workspace proxy, kept); minmax peak gains the value-extrema scalar
charges per batch; sparse peak stays `16 × input` — charged on **actual materialized bytes** at
task creation (the estimate feeds `max(per-operator estimates) + bytes_to_materialize`,
`gpu_pipeline_task.cpp:740-752`), which the plan-time byte gate below keeps schedulable and a
fired membership filter (§4.9) keeps small; it is not dropped for dense-committed forms because
sparse remains reachable (small-counted and nullable-argument regimes, §4.4) and the estimate
must cover whichever strategy runs. Same saturating arithmetic
(`src/include/memory/size_arithmetic.hpp`), same OOM retry floor
(`gpu_pipeline_task.hpp:93-130`), same allocator discipline (`rmm::exec_policy(stream, mr)`,
space taken from colocated input batches, .cpp:429-440).

**Downgrade/spill story (explicit, per R5):** unchanged from today, and justified: the operator
has **no CPU implementation** and is not downgrade-eligible; its FULL-barrier port repositories
are automatically spillable by the downgrade executor while waiting, and task preparation
re-materializes/colocates inputs into the reservation (`docs/super-sirius/memory-management.md`
contract). Bail-to-generic exists **only at plan time** (any ladder rung fails → generic
join+aggregate); post-plan the only runtime bail is dense→sparse, which is exact. The one new
memory risk — INNER forms colocating a huge counted side in one reservation (q17: ~96 GB at
SF1000) — is handled by a **plan-time byte gate** on the counted child's
`estimated_cardinality × row width`, failing closed to generic until the streaming seam lands.
The fraction is derived, not hand-waved: the reservation is
`max(dense, sparse, minmax) + bytes_to_materialize` with `sparse = 16 × bytes`, i.e. ~17× the
input must fit in usable device memory, so the gate is **counted-child estimated bytes ≤
device memory / 24** (GB300: 12 GB). Plan-time estimates do not see dynamic filters, so at
SF1000 q17's 96 GB counted estimate fails the gate and **P1 does not fuse at SF1000 before
PR-5** — plan-time refusal is the honest answer, stated as such in §5.1's measurement plan and
§9's PR-3 gate; at SF100 (~9.6 GB) it fuses and is measurable. A task whose *actual* bytes
grossly exceed the estimate remains the same exposure class as today's operator (which also
charges 16× on actuals) and is caught by the reservation math, not by a hang.

**Named seam: streamed accumulation (`BUILD_STREAM`).** The one-shot both-FULL schedule is a
policy choice, not a mechanism constraint: dense-array atomics make concurrent accumulation from
many tasks correct with zero merge logic. The seam: preserved port FULL → build state (extrema +
allocation) once; counted port PIPELINE → per-batch accumulate tasks pinned to the state's
device; emit when the counted producer finishes. Template: the hash join's
`per_partition_build_state` machine (`src/include/op/sirius_physical_hash_join.hpp:418-434` —
atomic build-state, device-pinned slots). This removes the counted-side colocation gate (and
with it the reason P1 cannot fuse at SF1000) and is scheduled as its own phase (§9, PR-5); the
count pathway never switches (R1). Two commitments the seam must honor, resolved now: streamed
specs fix `matched` at **u64** (counted row count is unknown at allocation time; +4 B/slot buys
the safe bound); and PR-5 must restate the memory estimate for streamed specs — the strategy
commits at build time, sparse partials are already per-batch (they stream structurally), so the
16×-all-input term is replaced by per-task charges plus the final merge/emit charge.

### 4.9 Integration inventory

Every arm the enum rename touches, with the intended new behavior:

| integration point | anchor | GROUP_JOIN behavior |
|---|---|---|
| converter port/barrier wiring | `sirius_pipeline_converter.cpp:211-218` (generic; consumes the op's virtuals) | unchanged |
| merge-fusion exclusion | `sirius_physical_plan_generator.cpp:858` | still excluded (bespoke multi-port sink) |
| narrowing policy | `sirius_plan_narrowing_policy.cpp:439-457` | **per-form arms** (today's arm hard-guards `children.size() != 2` at :440 and indexes `child_maps[0]/[1]` by position): two-child forms keep keys `boundary_restore`, COUNT arg narrow (validity-only), **SUM/MIN/MAX/AVG args value-sensitive ⇒ restore native** (rule precedent :479-505), no forwarded carrier; DIRECT adds a single-child arm — `child_maps[0]` only, native group key + value arg |
| compressed schema | `sirius_plan_compressed_schema.cpp:392-410` | **per-form arms** (today's arm hard-guards `children.size() != 2` at :393): two-child forms native keys + native value args; DIRECT single-child arm restores native group key + value arg on the sole child; `set_physical_types({})` in all forms |
| delim machinery | `sirius_plan_delim_join.cpp:50-69` (`gather_delim_scans` walks physical children) | `gather_delim_scans` still finds the `DELIM_SCAN` leaf as `children[0]` (child retained in the physical tree), so delim-index tagging is unmodified — but pipeline construction and port wiring for that child are **not** today's bodies; they take the routing-only arm of §4.8's provenance wiring (dependency registration via the scan's own `build_pipelines`, "preserved"-port mapping for the distinct-chain-root producer) |
| task creator | hint-chain driven, no special case (docs/super-sirius/task-creator.md) | unchanged |
| dynamic filters | hash-join build publication (`docs/super-sirius/dynamic-filters.md`) | Sirius filters are **row-level membership masks** (exact raw IN-list, hash IN-list, Bloom) applied post-decode at the scan — not zone maps (a separate opt-in). A DELIM_GET build side qualifies as publication evidence today (`build_relation_is_opaque`, `build_filter_evidence.cpp:38-48`, consumed at `sirius_plan_comparison_join.cpp:439-441`), so P1's fused join deletes a filter that cuts q17's lineitem from ~6 B to ~16 M rows independent of clustering. **Preserved-port publication is therefore a P1 prerequisite, not a fallback seam**: the fused op has the complete preserved side at its FULL-barrier port at the same point the hash join has its build; publication triggers on preserved-port producer completion, reusing the per-operator publication hooks (`sirius_physical_hash_join.hpp:472-497` precedent). Quantified per-pathway in §5.1. *Realized (PR-3):* the planning side is extracted as a shared single-key helper, `plan_single_key_membership_publication` (`src/planner/dynamic_filter/dynamic_filter_publication_planning.*`), fed by the same evidence (`build_subtree_is_filtering`/`build_relation_is_opaque` on `children[1]`), admission, target-discovery, and endpoint-placement mechanisms as the hash join, with probe=counted / build=preserved orientation; build evidence when the preserved side is *not* `children[1]` declines fusion (the replaced join would have published counted-side filters the fused op cannot — the no-downgrade rule). The runtime is the hash join's one-shot CAS state machine, triggered from the first task-hint poll after the preserved producer pipeline finishes, publishing only from a single whole-preserved GPU-resident delivery; for a FULL one-shot port there is no later delivery, so a non-resident or multi-batch preserved side closes the window permanently (within documented best-effort semantics). Follow-up: re-home `plan_comparison_join`'s inline target-discovery loop onto the shared helper |

---

## 5. TPC-H pathway specifications

Cardinalities: SF1000 projections from `notes-tpch-patterns` (verified against SF0.1 optimized
logical plans); all tpch-ext keys BIGINT.

### 5.0 P0 — q13 COUNT over outer join (existing; the preservation baseline)

Everything in §3, unchanged. Spec: `{OUTER_PRESERVING, COUNT bundle, dense budget 2 GiB}`.
c_custkey dense 1..150 M (1.2 GB dual-uint32 histogram), counted side orders 1.5 B rows,
σ_S = 100% (sparse fallback benign). This pathway defines the R1 contract (§7).

### 5.1 P1 — q17: AVG per delim partkey over an INNER correlation join

**Logical pattern** (verified, `scratchpad/tpch-plans/q17.logical.txt`): inside the RIGHT
delim join's LHS:

```
PROJECTION[0.2*avg(l_quantity), p_partkey]
  AGGREGATE[groups: p_partkey; avg(l_quantity)]          ← fusion root
    COMPARISON_JOIN INNER (l_partkey = p_partkey)
      SEQ_SCAN lineitem (full)                           ← counted side
      DELIM_GET (~540 K distinct p_partkeys @SF1000)     ← preserved side
```

**Detection rule (ladder rung P1):** 1 group (BOUND_REF), 1 aggregate ∈ exact-builtin
{avg, sum, min, max, count, count(*)} with bare-BOUND_REF argument, no
DISTINCT/FILTER/ORDER BY; child `LOGICAL_COMPARISON_JOIN` with `JoinType::INNER`, 1
COMPARE_EQUAL, no residual, projection maps validated (reuse :143-173); group ref resolves to
one side's join key — that side is *preserved* and must be a childless `LOGICAL_DELIM_GET` **or**
a linear scan chain whose key passes `prove_unique_columns` (delim provenance is the primary
target; the uniqueness requirement here is not for correctness — §2.3 — but as the σ-asymmetry
evidence justifying the relaxed gate table); other side (*counted*) is a linear scan chain;
aggregate argument resolves to the counted side; keys both INT32 or both INT64; argument type ∈
{INT32, INT64, DECIMAL32, DECIMAL64}; counted-side plan-time byte gate (§4.8) passes; the
preserved-key membership publication plan (§4.9) is installed with the fusion — the fused plan
must not be a filter downgrade relative to the plan it replaces.

*Realized (PR-3) — fail-closed widenings the verbatim rule needed to match real q17 plans:*
(i) zero or more intervening bare column-selection projections between the aggregate and the
join (`RemoveUnusedColumns` leaves them at SF100+) are composed into a reference remap; any
projection that computes, or any ill-typed hop, is a miss. (ii) Empty `grouping_sets` is
accepted alongside the singleton set — subquery flattening appends the correlation group to an
originally ungrouped aggregate, leaving `grouping_sets` empty, which is plain GROUP BY
semantics. (iii) When the group binds to the side without preserved provenance but the other
side qualifies, the group is re-homed onto the other side's key — value-safe because a single
plain `COMPARE_EQUAL` on identical unparameterized INTEGER/BIGINT types makes the two key
columns pairwise equal on every emitted row. (iv) The declared aggregate output type is screened
against what the fused operator can emit, because the operator validates its spec by throwing
and a planner throw would fall the whole query to CPU instead of to generic GPU planning.

**Spec/state:** `{INNER, AVG bundle}` — arrays presence + matched + sum:i64 with per-array
widths (§4.4); argument nullability decided from batch metadata (§4.2; l_quantity is NOT NULL,
so the gate never fires here). Runtime domain: p_partkey extrema ⊆ [1, 200 M] ⇒ range ≈ 200 M.
Two runtime regimes, both exact:

- *Membership filter missed/late/off:* counted ≈ 6 B rows ≥ 2^32 ⇒ matched:u64 ⇒
  4+8+8 = 20 B/slot ⇒ state ≈ 4.0 GB ≤ 16 GiB budget; gates (a),(d) pass, (e) passes
  (4.0 GB ≤ 4 × ~96 GB input); gate (c) dropped for this form (§4.4) ⇒ **dense**.
- *Membership filter fired:* counted ≈ 16 M rows ⇒ matched:u32 ⇒ 16 B/slot ⇒ 3.2 GB, but gate
  (e) now fails (3.2 GB ≫ 4 × ~0.3 GB input) ⇒ **sparse** — which is exactly right: post-filter
  the shape is q13-like (σ_S ≈ 100%, every surviving row joins), the eager hazard is gone, and
  a 16 M-row groupby is cheap. The gate table adapts per-regime by construction.

**Kernel/state deltas vs COUNT path:** counted pass adds one `atomicAdd(int64)` and one 8 B
argument read per row; extrema pass additionally reduces the argument column min/max, merged
into the same device array and the same single sync; emit adds the `matched > 0` conjunct to the
selection predicate and finalizes with one `cudf::binary_operation` DIV (mirroring
`sirius_physical_grouped_aggregate_merge.cpp:259-284` — DuckDB's declared avg return type
decides DECIMAL fixed-point vs FLOAT64). Preserved pass, launch geometry, selection scan,
NULL handling: identical code.

**Replaced fragment:** hash-join build (540 K) + probe (6 B rows unfiltered; ~16 M when the
baseline's membership filter fires) + ~16 M-row × 2-col gather + HASH_GROUP_BY + PARTITION +
MERGE_GROUP_BY (≈ 6–8 pipelines) → 3 pipelines, 1 task. With preserved-port publication as a
prerequisite, the fused plan sees the same filtered counted stream the baseline does — the
comparison stays apples-to-apples in both regimes.

**Expected effect and where it is measurable:** the win is the eliminated join-output
materialization, the eliminated group-by fragment, and ~4 fewer task/repository round-trips and
inter-pipeline gaps — the same phase-serialization overhead class identified as a top cost in
the q9/q21 profiles; the counted-side pass itself (filtered: ~16 M rows either way; unfiltered:
6 B-row scattered atomics vs 6 B-row L2-resident probe + materialization, §4.4) is the term the
estimate is conditional on. **At SF1000 the one-shot schedule cannot be admitted** (96 GB
counted estimate vs the §4.8 byte gate; and 16 × 96 GB is unschedulable in 288 GB regardless),
so PR-3 measures at SF100 (~9.6 GB counted estimate, fuses) plus an SF1000 no-regression run in
which fusion correctly declines; the **−10…25% q17 wall-time estimate belongs to the PR-5
(BUILD_STREAM) SF1000 measurement**, conditional on (a) filter parity holding and (b) the
scattered-atomic microbenchmark (§4.4) confirming the accumulate pass is not the new critical
path. Realistic floor in every regime: neutral-if-scan-bound. All in the PR #1371 kit regime.

**Gating (fail-closed):** `enable_group_join`; every detection miss → generic; counted-byte
plan gate → generic (this is what declines SF1000 pre-PR-5); runtime dense decline →
sparse-exact (correct; benign in the filtered regime; slow only for adversarial
extrema — p_partkey cannot exceed [1, 200 M·SF/1000]).

**Risks:** (1) one-shot colocation of the counted side — refused at plan time by the byte gate,
removed by PR-5 streaming; (2) dynamic-filter parity — the baseline's delim-side hash join is
publication-eligible today (membership masks, not zone maps; §4.9), so preserved-port
publication is a P1 *prerequisite*; residual risk is publication timing (the counted scan may
finish before the preserved side publishes — same best-effort semantics as today's late
filters), measured in PR-3's kit run; (3) sparse fallback = eager 6 B-row groupby if gates are
mis-tuned — bounded by the gate table and the SF-scaling note in §11; (4) DECIMAL scale/typing
mistakes — covered by the oracle tests (§10).

### 5.2 P2 — q2: MIN per partkey as the DIRECT form

**Logical pattern** (from `q2.logical.txt`): inside the LEFT delim join's RHS:

```
PROJECTION[min(ps_supplycost), p_partkey]
  AGGREGATE[groups: p_partkey; min(ps_supplycost)]       ← fusion root
    COMPARISON_JOIN INNER (ps_suppkey = s_suppkey)       ← group key NOT this join's key
      COMPARISON_JOIN INNER (ps_partkey = p_partkey)
        partsupp chain, DELIM_GET(~800 K partkeys)
      CTE_SCAN (eligible suppliers)
```

**Why not a two-input groupjoin:** the group key binds the *lower* join. Fusing there would
delete the delim-key semi-filter (partsupp 800 M → ~3.2 M rows) and force the supplier join to
process 800 M rows — a pessimization; fusing at the upper join violates M11 precondition 1
(`{p_partkey} ↛ {s_suppkey}`). The honest minimal mechanism: keep the join tree exactly as
planned and fuse **only the aggregate** as the DIRECT form — the counted input is the whole
subtree, planned by `create_plan` as-is (no linear-chain requirement, no re-rooting, no
synthesized second DELIM_SCAN). Γ semantics directly; presence array and pass deleted.

**Detection rule (ladder rung P2):** 1 group (BOUND_REF, INT32/INT64), 1 supported aggregate
with bare-BOUND_REF argument; child is `LOGICAL_COMPARISON_JOIN` (any join type — the child is
opaque, which is *sound* only because the argument-validity gate (§4.2/§4.5) routes any
padding-NULL arguments an outer join inside the child produces to the mask-preserving sparse
path; requiring a join root confines the pathway to fragments where a fused join+agg shape was
the alternative, keeping us out of generic HASH_GROUP_BY/perfect-hash territory); child planned
via `create_plan` unchanged, attached via the single-child sink build path (§4.8 provenance
class 3 — today's two-child `build_pipelines` cannot take an opaque join-rooted child).
Group-key and argument indices taken directly from the aggregate's input references
(post-`extract_aggregate_expressions` hoisting guarantees bare refs, :81-129).

**Spec/state — q2 runs the sparse strategy, by design of its own gates:** the counted subtree
output is ~640 K rows (~10 MB) against a p_partkey domain of ~200 M, a live-key/domain ratio
that is scale-invariant; gate (d) needs range ≤ 2 × total rows (off by ~156×) and gate (e)
needs state ≤ 4 × input bytes (2.4 GB vs ~40 MB, off by ~60×), so the dense histogram is
rejected at every SF — **correctly**: filling and scanning gigabytes of state for 640 K rows
would be a pessimization. What q2 actually executes is `{DIRECT, MIN bundle}` on the **sparse
path inside the single fused task**: one `cudf::groupby` MIN with NULL-inclusive keys (the
NULL group, §4.4), duplicate-agnostic emit (no ⊗), ps_supplycost DECIMAL(15,2) compared as
unscaled int64 (order-preserving for a fixed scale), declared decimal type on emit. The dense
MIN machinery (sentinel `thrust::fill` init, `atomicMin`, NULL-key extra slot at index `range`)
ships in the same bundle because it **is planner-reachable** — rung P1 admits min/max over
delim-INNER shapes and rung P2 admits dense-domain DIRECT shapes; neither happens to be
instantiated by a TPC-H query, so its reachability proof is a dedicated dense-forcing SQLLogic
query through the full planner (asserted via `last_strategy() == DENSE`, §10), not a synthetic
Catch2-only exercise.

**Deltas vs COUNT:** sparse partials swap COUNT for MIN in the groupby request; no presence
side, no ⊗ multiply (duplicate-agnostic); the dense MIN kernel deltas (fill-sentinel init, one
`atomicMin` + argument read per row) are as specified in §4.2 and reached by the dense-forcing
shapes above.

**Expected SF1000 effect:** small and honest: this replaces a cheap HASH_GROUP_BY + PARTITION +
MERGE fragment (3 pipelines, 2 barriers) with one sink task — an overhead/latency win in the
~10–30 ms class on a query whose cost lives in the shared CTE join tree. Value of the pathway
is (a) breadth proof for the MIN bundle (sparse) and the DIRECT form at near-zero risk, (b) the
DIRECT machinery itself, which any future dense-grouped-aggregate work reuses. Accept "neutral,
no regressions" as its gate.

**Gating/risks:** `enable_group_join`; byte gate on the child estimate; runtime dense/sparse as
usual. Risk: DIRECT territory overlap with future perfect-hash aggregate work — bounded by the
join-root shape requirement; revisit when that work exists.

### 5.3 Why these two first

q17 exercises: INNER form, DELIM preserved side (including the routing-only wiring arm, §4.8),
AVG (payload + finalize), value extrema/overflow machinery, the relaxed gate table in both its
regimes (dense unfiltered, sparse filtered), and preserved-port filter publication. q2
exercises: DIRECT form (single-child build path), the sparse value path with MIN and
duplicate-agnostic ⊗, NULL-group semantics, opaque child subtrees. What no TPC-H query
instantiates — the dense MIN/MAX kernels (sentinel-init + atomicMin/Max) and the dense DIRECT
NULL-group slot — is planner-reachable through rungs P1 (min/max admitted) and P2
(dense-domain DIRECT shapes) and is covered by dense-forcing SQLLogic queries through the full
planner (§10). What remains uncovered (SUM standalone) is a strict subset of AVG's mechanism.

### 5.4 P3 — q18: SUM per orderkey with carried determined columns (spec now, build behind the seam)

**Pattern** (from `q18.logical.txt`): `AGGREGATE[groups: c_name, c_custkey, o_orderkey,
o_orderdate, o_totalprice; sum(l_quantity)]` over `INNER (l_orderkey = o_orderkey)` with
counted = lineitem (6 B rows) and preserved = `orders ⋉ qualifying-keys ⋈ customer` (~60 K rows
at SF1000).

**Detection rule:** multi-group variant — one group column resolves to the preserved join key;
every other group column resolves to the preserved side and is accepted only if the preserved
subtree proves the key functionally determines its output rows (`prove_unique_columns` over the
join tree, `sirius_plan_comparison_join.cpp:63-199`, i.e. M11 Eq 4/6/7 key-shrinking with the
FD proof made explicit — Appendix E counterexample 2 is the silent-wrong-answer risk this proof
exists to kill). Aggregate = SUM from the counted side.

**Why blocked on the seam:** o_orderkey spans ~6e9 ⇒ `dense_direct` state ≥ 48 GB fails every
gate; the preserved side is tiny ⇒ `preserved_remap` (§4.4) is the right strategy, and it also
makes carried columns a plain gather (slot = build row). Nothing else is new: SUM bundle,
INNER form, ⊗ multiply already exist after P1/P2.

**Expected SF1000 effect:** modest — the 6 B-row probe remains either way; the win is the
eliminated ~420 K-row join gather and the 5-key (VARCHAR-bearing) group-by fragment. q18's
actual whale is the *scan-direct* `AGG[l_orderkey; sum]` HAVING feeder, which is not a
groupjoin and out of scope. **Decision gate:** build P3 only if PR-5 profiling shows the
fragment+gather share of q18 ≥ ~50 ms; otherwise the remap seam stays dormant until the
membership sibling or q20 justifies it.

**Risks:** FD-proof soundness (must be conservative; any non-BOUND_REF or unproven column ⇒
generic), remap probe cost vs the deleted hash join (≈ wash — same table, same probes),
dictionary cap (128 M precedent).

---

## 6. Framework interfaces — planner-to-execution contract

```mermaid
graph LR
  subgraph "pipelines (unchanged shape)"
    A[preserved producer<br/>DELIM_SCAN or scan chain] -- "FULL, port=preserved" --> GJ
    B[counted producer<br/>scan chain or opaque subtree] -- "FULL, port=counted" --> GJ
    GJ[GROUP_JOIN<br/>one task] --> C[consumer pipeline]
  end
```

- Planner emits `group_join_spec` (§4.2); the operator stores it; `execute` maps
  `(form, slots, key type, slot widths)` to one entry of an **explicit host-side dispatch
  table** of monomorphized instantiations (enum → function pointer; the whitelist is the table).
  No virtual dispatch below `execute`.
- `input_port_for`/`input_barrier_for`/`build_pipelines`/`get_next_task_hint`/
  `get_next_task_input_data`/`no_history_peak_memory_estimate` retain today's signatures
  (`src/include/op/sirius_physical_operator.hpp:508-534`) and, for the count spec, today's
  bodies; the delim-fed and DIRECT specs take the provenance arms of §4.8 (routing-only
  `DELIM_SCAN` child; distinct-chain-root producer → "preserved" port; single-child sink build).
- The DIRECT form registers a single port; hint logic degenerates to "counted producer
  finished".

---

## 7. Performance preservation argument (R1)

Claim: for every plan the count pathway fuses today, the framework produces the same planner
decision, the same task structure, the same kernels, the same pass count, the same allocation
set, and the same synchronization count.

1. **Planner decision.** Ladder rung P0 is today's `detect_dense_count_join` checks verbatim,
   in today's order, behind today's config bool first (invariant: config → shape → catalog,
   `sirius_plan_aggregate.cpp:367-372, 242-360`). New rungs execute only after P0 has returned
   `nullopt` — i.e., only on shapes that today already fall through to generic planning — so
   count-eligible plans see zero added planner work before the decision and identical decisions.
   Gating is *strictly widened*: nothing that fused before stops fusing.
2. **Operator wiring.** Ports, barriers, `build_pipelines`, hint, and input-drain logic are the
   same code paths (§4.8); the spec for COUNT carries the same field values the current ctor
   takes (:258-276). One task, both FULL barriers, no repository round trips beyond the two
   ports.
3. **Strategy selection.** The count spec selects the count gate column of §4.4 — the same five
   conditions with the same constants and the same 2 GiB engine-owned budget
   (`sirius_config.hpp:103`), computed from the same extrema pass. The device extrema array
   stays 2 slots for bundles without SUM/AVG, so `dense_count_global_minmax` runs its current
   kernels and its **single** unconditional sync (impl.cu:441). The count spec keeps today's
   *joint* wide-slot promotion rule verbatim (either side ≥ 2^32 rows widens both arrays,
   .cpp:510-512); the per-array width rule of §4.4 applies to the new forms only, so the count
   path's slot layout is bit-identical.
4. **Kernels.** `accumulate_kernel<KeyT, count_bundle>` contains one slot policy whose `update`
   is today's `histogram_add` (impl.cu:118-124); with `no_arg`, the argument load and the
   second atomic fold away at instantiation — the loop body is source-identical to
   impl.cu:126-155 after inlining, with the same mask checks, the same unsigned-offset
   arithmetic, the same `bounds_check` asymmetry, and the same launch constants
   (`k_block_size = 256`, `k_max_blocks = 4096`, impl.cu:51-59). Emit keeps
   `presence_positive`, `count_if` + `copy_if` under `rmm::exec_policy(stream, mr)`, and the
   count `emit_kernel` body including `matched = max(matched, 1)` and the conditional overflow
   flag (impl.cu:157-189, 315-391). **Verification is part of the Phase-1 gate, not an
   assumption:** (a) `cuobjdump --dump-sass` diff of the count instantiations against the
   pre-refactor binary (accept identical or reviewed register-allocation-only deltas); (b)
   allocation-set parity from reservation logs (2 histograms + memsets, selected uvector, 2
   output columns, per-batch minmax scalars, optional overflow scalar); (c) SF1000 kit A/B
   within run noise (§9).
5. **Sparse path and NULL handling.** Untouched code for the count bundle
   (.cpp:551-651, impl.cu:263-313, 578-605).
6. **Memory estimate.** The count-bundle formula evaluates to today's expression term-for-term
   (:365-409) — same `min(cap, 4×bytes)` histogram charge, same double-charge CUB proxy, same
   scalar accounting — so admission and OOM-retry behavior are unchanged.
7. **Integration arms.** The enum rename is mechanical; each arm's count behavior is preserved
   by construction (§4.9), including narrow COUNT-arg transport.

Added per-row overhead on the count hot path: **zero** — no new branches, loads, or dispatch
reach the instantiated count kernels.

---

## 8. Non-goals (v1)

| cut | justification |
|---|---|
| DISTINCT aggregates | COLLECT_SET/MERGE_SETS machinery is heavyweight and no pathway needs it (`aggregate_op_util.cpp:101-131` shows the cost) |
| ordered / FILTER-clause aggregates | rejected today (:261); rare, and DuckDB rewrites ordered aggregates into shapes we refuse anyway |
| multiple aggregates per fusion | mechanism takes N slots; detection caps at 1 to bound kernel instantiations — lifting it is a whitelist entry, not a design change |
| value aggregates over OUTER joins | needs aggregate-output null masks; no pathway (q13 is COUNT; q17/q2 are INNER/DIRECT) — §4.5 seam |
| composite keys (q20) | dense composite domains don't exist; needs `preserved_remap`; single pathway, low urgency |
| arbitrary sparse/hash domains | `preserved_remap` is a designed seam (§4.4), built only when P3's decision gate passes |
| SEMI/ANTI dense membership (q4/q22) | different operator family (bitmap at the delim join); shares only the domain machinery |
| streamed / multi-GPU accumulation | `BUILD_STREAM` seam (§4.8), PR-5; multi-GPU stays single-device like every single-task op |
| CPU downgrade of the operator | plan-time bail + data-tier spill covers it (§4.8); a CPU groupjoin duplicates DuckDB |
| groupjoin-aware join ordering | M11's Q9 caveat accepted; we pattern-match optimized plans, fail closed, and measure |
| aggregate cardinality estimates (F21 §4) | needs a stats subsystem; our gates are stats-free by design |
| non-integer keys (DATE/VARCHAR) | no pathway; DATE is a cheap later widening of the key dispatch |

---

## 9. Phased implementation plan

Each PR is independently landable, benchmarkable in the PR #1371 kit regime (per the
measurement-regimes protocol), and fail-closed by default.

| PR | content | gate to merge |
|---|---|---|
| **PR-1** | Mechanical generalization: `GROUP_JOIN` enum + `sirius_physical_group_join` + spec/bundle types; count bundle only; detection restructured into the ladder with P0 verbatim; all integration arms updated; docs updated | SASS parity on count kernels; allocation-set parity; full unit suite + Super-Sirius SQL coverage (the legacy-only `test/sql/` harness is waived, see §10); **SF1000 kit A/B neutral within run noise**; plan-parity check (EXPLAIN comparison, feature config unchanged) on all 22 queries |
| **PR-2** | Value slot policies (SUM/MIN/MAX/AVG), INNER + DIRECT forms, value-extrema fold-in, sparse-value fallback, overflow policies, NULL-group slot; **no planner rung enabled** — Catch2 drives the executor directly | Catch2 kernel/operator suites incl. oracle comparisons; kit neutral by construction (plan parity asserted) |
| **PR-3** | P1 (q17) detection rung behind `enable_group_join` (default off); counted-byte plan gate (device-mem/24, §4.8); **preserved-port membership publication** (§4.9 prerequisite); delim-fed wiring arms + fused-under-delim conversion test (§4.8); the two-child value-sensitive narrowing/compressed-schema arms (§4.9 — pulled forward from PR-4 as a correctness prerequisite for value fusion); SQLLogic + oracle tests | scattered-atomic microbenchmark (§4.4) run and archived; **SF100** kit A/B with the knob on: q17 fuses (verified in the log) and improves or is neutral; **SF1000**: fusion declines by design (byte gate) — assert plan parity and no regression on the 22-query suite (leave-one-out discipline); then flip default on. The SF1000 q17 win is *not* claimable here |
| **PR-4** | P2 (q2) DIRECT rung; single-child sink build path + the single-child narrowing/compressed-schema arms (§4.8/§4.9; the two-child value arms landed in PR-3); dense-forcing DIRECT reachability test (§10) | same protocol; accept neutral for q2 given no suite regressions |
| **PR-5** | `BUILD_STREAM` schedule for INNER/DIRECT (hash-join build-state pattern; streamed `matched` fixed at u64, §4.8); remove the counted-byte gate and restate the memory estimate for streaming-capable specs; count pathway untouched | **SF1000** kit A/B: q17 fuses and shows the §5.1 estimate (−10…25%, floor neutral-if-scan-bound) conditional on filter parity + the microbenchmark; memory profile (reservation sizes, downgrade counts) improves; wall time ≥ PR-3 result |
| **PR-6** (decision-gated) | `preserved_remap` + carried determined columns → P3 (q18), possibly q20 | go/no-go on PR-5 profiling evidence (§5.4); then the standard protocol |

Rollback story at every stage: one config bool (`enable_group_join`) returns planning to
bit-identical-to-today behavior; PR-1's refactor itself is guarded by its parity gates.

**Delivery status (2026-08-25, PR-4 audit):** PR-1 through PR-4 are delivered and their merge
gates passed; all three v1 pathways are live — P0 (q13 COUNT, R1 baseline preserved: SASS 24/24
identical, plan/result parity), P1 (q17 INNER AVG: fuses at SF100 with publication, declines at
SF1000 by the byte gate as designed), P2 (q2 DIRECT MIN: fuses at SF100 on the sparse strategy
exactly per §5.2, accepted neutral). The genericity claim is proven at v1 scope: PR-4 added a
whole join form through the form-indexed seams alone (detection rung, one wiring arm, per-form
narrowing/compressed-schema arms, spec validation) with **zero device-code changes** — the
bundle, emit, strategy, and memory-estimate axes were untouched. PR-5 (`BUILD_STREAM`) and
PR-6 (decision-gated `preserved_remap`) remain. Carried to PR-5, non-blocking:
(i) `docs/super-sirius/operators.md` still describes two wired pathways — extend with rung P2;
(ii) archive the knob-off-vs-HEAD plan-parity artifact in the PR-4 evidence set (the property
itself was independently verified in review: 22/22 plans, results md5-equal vs PR-3-A);
(iii) a breadcrumb comment at the base `is_sink()` GROUP_JOIN clause noting the merge-fusion
dead-end walk now terminates at the fused op's child.

---

## 10. Test plan

**Correctness oracle (every pathway):** the same query with (a) `enable_group_join = false`
(Sirius generic GPU path) and (b) `gpu_execution = false` (DuckDB CPU). The SQL-level tests
below assert identical results; Catch2 operator tests compare tables directly.

**SQL-level tests per pathway.** The existing `test/sql/*.test` SQLLogic files drive only the
legacy `gpu_processing` engine (`gpu_processing`/`gpu_buffer_init` are registered only under
`SIRIUS_ENABLE_LEGACY`, which is OFF in the build and in CI), so pathway SQL tests land as Catch2
integration SQL through transparent interception (the `test_gpu_execution_group_join.cpp`
pattern), covering:

- empty inputs: empty preserved / empty counted / both; empty delim buffer (zero correlation
  keys) for P1;
- empty groups: preserved keys with no counted match — COUNT(col)=0 and COUNT(*)=1 on P0
  (the COUNT bug rows); dropped on INNER/DIRECT;
- NULL keys: NULL preserved keys (single NULL group), NULL counted keys (skipped on joins,
  grouped on DIRECT), NULL aggregate arguments (skipped; COUNT(*) counts);
- NULL-argument groups (§4.2/§4.5 gate): groups whose matches exist but all arguments are NULL —
  INNER: row kept with COUNT(col)=0 / SUM=NULL; DIRECT over an outer-join child
  (`customer LEFT JOIN orders` under SUM/MIN GROUP BY): padded groups emit NULL, assert
  `last_strategy() == SPARSE`, results vs the CPU oracle;
- dense-MIN reachability: a dense-domain MIN query through the full planner (P1 min/max rung
  and a dense-domain DIRECT shape) asserting `last_strategy() == DENSE` — the dense sentinel /
  atomicMin / NULL-group-slot machinery must be reached from SQL, not only from Catch2;
- duplicate preserved keys (⊗ scaling: COUNT/SUM scale, MIN doesn't, AVG invariant; NULL ⊗ c =
  NULL on the sparse path);
- overflow: SUM near INT64_MAX (dense declines → sparse; result matches generic), COUNT
  product overflow (existing tests extended);
- domain-cap bail: test-hook `group_join_max_state_bytes` forced tiny ⇒ `last_strategy() ==
  SPARSE` and results identical (pattern: existing `scoped_sirius_setting` +
  `dense_count_join_max_bytes` hook);
- DECIMAL scales for SUM/MIN/AVG, AVG return-type parity with DuckDB;
- q13/q17/q2 (and q18 in PR-6) full-query results vs feature-off.

**Catch2:**

- `test/cpp/planner/test_group_join_detection.cpp` — extend the 468-line
  `test_dense_count_join_detection.cpp` pattern per rung: positive shapes plus fail-closed
  negatives (DISTINCT/FILTER/ORDER BY, UDF name collision on avg/sum/min/max, wrong-side group
  key, expression keys, multi-condition joins, residual predicates, INDF conditions, non-INT
  keys, projection-map traps, non-DELIM non-unique preserved side for P1, multi-group for
  P1/P2);
- operator tests (`test_physical_group_join.cpp`): per-bundle dense/sparse forcing, strategy
  assertion via `last_strategy()`, NULL-group slot, sentinel-init MIN, AVG finalize both type
  branches, ⊗ scaling, peak-estimate ≥ observed allocation property test;
- GPU execution tests (`test_gpu_execution_group_join.cpp`): downgrade participation — spill
  the FULL-barrier port repositories to HOST/DISK under pressure and verify re-materialization;
  OOM retry — undersized reservation ⇒ `oom_reschedule_exception` path exercises the retry
  floor (`gpu_pipeline_task.hpp:93-130`) and succeeds;
- conversion tests (PR-3/PR-4 gates): the fused-under-delim shape converts — DELIM_SCAN child
  registers its distinct-chain dependency, no sourceless pipeline exists, the distinct-root
  producer resolves to the "preserved" port, and the task fires after both producers (§4.8
  provenance class 2); the DIRECT shape converts via the single-child sink path (class 3);
- PR-1 parity harness: count-bundle kernel outputs bit-compared against recorded pre-refactor
  outputs across the dense/sparse/NULL/overflow matrix.

**Performance:** kit A/B per the merge gates in §9 (SF100 for PR-3, SF1000 from PR-5); the
scattered-atomic microbenchmark (6 B random atomicAdds over a 3.2–4 GB device array, §4.4)
archived before PR-3 merges; per-query nsys spot checks that the fused task shows the expected
pass structure (one accumulate launch per batch per side, one count_if/copy_if pair, one emit)
and exactly one sync on the new pathways.

---

## 11. Open questions

1. **DuckDB `avg(DECIMAL)` declared type** — DECIMAL vs DOUBLE decides which finalize branch
   P1 exercises on q17; resolve empirically in PR-2 tests (both branches implemented either
   way, mirroring `sirius_physical_grouped_aggregate_merge.cpp:268-284`).
2. **Plan-parity tooling** — is there an existing EXPLAIN-hash harness for the 22-query
   plan-parity gate, or does PR-1 add a small one?
3. **Dynamic-filter effectiveness on q17** (§4.9): eligibility is settled — the delim-side
   build qualifies today (`build_relation_is_opaque`), so preserved-port publication is a P1
   prerequisite, not an open design choice. What PR-3's kit run must still quantify is
   *effectiveness*: how often the filter lands before the counted scan finishes (publication
   timing), and the measured row cut, to validate §5.1's filtered-regime assumptions.
4. **SF-scaling cliff of the relaxed budget:** with the per-array widths (§4.4) and the
   `min(16 GiB, device/16)` budget, q17-class state fits through SF3000 (~12 GB unfiltered) on
   GB300-class HBM; beyond that, or on smaller devices, dense declines ⇒ sparse ⇒ eager hazard
   in the unfiltered regime (the filtered regime is benign, §5.1). Option when it bites: pull
   `preserved_remap` forward. Decide on evidence, not now.
5. **DIRECT-form breadth:** should PR-4's rung later accept non-join children (pure dense
   group-by, q15-adjacent)? Deliberately out of scope until the perfect-hash aggregate
   direction is settled; revisit with the aggregation owners.

---

## Design review resolutions

- [performance/blocker] q17 slot-width math contradicts the retained wide-promotion rule -> fixed: per-array width rule for new forms (presence from preserved rows, matched from counted rows, payloads always 64-bit; same per-key ≤ side-rows proof, no saturation flag), §4.4; count bundle keeps the joint rule verbatim, §7.3; budget re-derived at SF1000–3000 with the corrected 16–20 B/slot widths and the default raised to `min(16 GiB, device/16)`, §4.7/§5.1.
- [performance/major] P1's one-shot schedule cannot be admitted at SF1000 -> fixed: byte-gate fraction specified and derived (counted estimate ≤ device/24 from the 17×-input reservation math), plan-time refusal stated as the design (P1 does not fuse at SF1000 before PR-5), PR-3 gate restructured to SF100 measurement + SF1000 plan-parity, the −10…25% claim moved to PR-5's SF1000 gate; 16× sparse charge retained (on actuals, bounded by the gate; sparse stays reachable per-regime), §4.8/§5.1/§9.
- [performance/major] Dynamic-filter loss dismissed with a wrong mechanism description -> fixed: mechanism corrected to row-level membership masks (raw/hash IN-list, Bloom; zone maps separate opt-in), verified that the q17 delim build side is publication-eligible today (`build_relation_is_opaque`, build_filter_evidence.cpp:38-48 → sirius_plan_comparison_join.cpp:439-441), and preserved-port publication promoted from fallback seam to P1 prerequisite (publish on preserved FULL-port completion), §4.9/§5.1/§9; open question 3 narrowed to publication-timing effectiveness.
- [performance/major] q2's dense DIRECT path is unreachable under the doc's own gate table -> fixed: §5.2 rewritten — q2 runs the sparse strategy inside the single fused task at every SF (gates (d)/(e) correctly reject a 2.4 GB histogram for 640 K rows), which still delivers the fragment-collapse win; dense MIN machinery retained because rungs P1 (min/max admitted) and P2 (dense-domain DIRECT) make it planner-reachable, with a dense-forcing SQLLogic reachability test added as its proof, §5.2/§5.3/§10.
- [performance/major] Gate-(c)-drop cost argument omits the scattered-atomic term -> fixed: §4.4's economics rewritten with the dominant term (N_counted × 2–3 DRAM-random atomics over a ≫L2 state region, ~100 ms-class at 6 B rows) vs the L2-resident probe and the filtered baseline; the drop remains justified for the dense-vs-sparse decision it governs, the fused-vs-unfused estimate is now explicitly conditional, and a 6 B-atomicAdd microbenchmark is a PR-3 merge prerequisite, §4.4/§5.1/§9/§10.
- [semantics/blocker] P2 DIRECT's "any join type" child re-admits value-over-outer NULL semantics -> fixed: argument-validity runtime gate (§4.2) — dense requires argument `null_count() == 0` across all harvested batches, so padding NULLs from any outer join inside the opaque child route to the sparse mask-preserving path, which emits SUM/MIN/etc. = NULL correctly; the child stays opaque, semantics table gains the padded-group treatment and a DIRECT-over-LEFT-JOIN oracle test, §4.2/§4.5/§5.2/§10.
- [semantics/major] `matched` conflates c2 with the valid-arg count; no handling for all-NULL-argument groups -> fixed: the same argument-validity gate makes valid ≡ matched ≡ c2 provably equal on the dense path (also deleting AVG's valid_cnt array); the sparse path computes both COUNT(arg) and COUNT(*) per key for nullable-argument INNER and carries output validity masks through merge and ⊗; §4.5 gains the missing "key matches, zero valid arguments" row; separate-c2 dense state stays a named seam, §4.2/§4.4/§4.5.
- [architecture/blocker] Delim-fed preserved-side wiring refuted by source -> fixed: new "wiring by input provenance" subsection (§4.8) — routing-only DELIM_SCAN child gets `build_pipelines` invoked in place (dependency registration, no producer pipeline); `input_port_for` gains an owning-delim distinct-chain-root arm mapping to "preserved" (hash-join CONCAT precedent), fail-closed otherwise; hint semantics restated for the MERGE_GROUP_BY src_pipeline; §4.9's delim row corrected from "unmodified" and a fused-under-delim conversion test added to PR-3's gate, §4.8/§4.9/§9/§10.
- [architecture/major] DIRECT cannot reuse today's build_pipelines or 2-child-guarded arms -> fixed: DIRECT specified as the standard single-child sink pattern (base-class shape, operator.cpp:167-181) in §4.8 provenance class 3; §4.9's narrowing/compressed-schema rows now specify per-form arms including the single-child variant; PR-4's scope updated, §4.8/§4.9/§5.2/§9.
- [mechanism/minor, PR-2 audit] §4.4's sparse delta (i) prescribed materializing COUNT(*) as c2 for nullable-argument INNER -> amended: the implementation realizes σ_{c2>0} structurally through the distinct-key inner join (a counted-partial key exists iff ≥1 key-match row), which is exactly equivalent because c2's value is emitted only for COUNT(*), an op that takes no argument and thus never meets the nullable-argument regime; §4.5's last-row semantics verified cell-for-cell against this mechanism (row kept, COUNT(col)=0, value aggs NULL through merge and ⊗), §4.4.
- [mechanism/minor, PR-2 audit] §4.8's sketch extended `group_join_state<KeyT, Bundle>` to the value bundles -> amended: value forms ship as two per-form dense drivers with a host-side `dense_value_op` switch over monomorphized kernels (16+8 instantiation whitelist instead of ×5 op multiplication; shared `accumulate_counted_form` unifies INNER's NULL-skip and DIRECT's NULL-group slot as one kernel via `null_slot`); the device hot path stays statically dispatched per §4.1, the count-bundle state class is untouched per R1, and the drivers' internal allocate→accumulate→emit phasing keeps the PR-5 BUILD_STREAM split mechanical, §4.8.
- [consistency/minor, PR-2 audit] §4.2's bundle table listed the MIN/MAX extreme array as int64/int32, contradicting §4.4's payloads-always-64-bit width rule -> amended: always int64 (the implementation follows §4.4; a 32-bit extreme array is a possible future footprint optimization, not a shipped width), §4.2.
- [estimate/minor, PR-2 audit] §4.8's AVG finalize charge said "one extra group-sized column" -> amended: the implementation charges the divisor plus the FLOAT64 branch's two cast temporaries (3 group-sized INT64 columns + the declared-type output), validated by the per-bundle estimate ≥ observed-allocation property test, §4.8.
- [config/minor, PR-2 audit] §4.7's `min(16 GiB, device memory / 16)` default left multi-device and no-device behavior unspecified -> amended: minimum over all visible devices' total memory (conservative for heterogeneous rigs), falling back to the 2 GiB count-form budget when no device is visible; YAML key rejected and SQL setting internal, exactly per the `dense_count_join_max_bytes` pattern, §4.7.
- [process/minor, PR-1 audit] PR-1's SQLLogic merge gate targeted a harness the build cannot run -> amended: `test/sql/*.test` drives only the legacy `gpu_processing` engine (both table functions registered under `SIRIUS_ENABLE_LEGACY`, OFF in the build and in CI), so the PR-1 gate substitutes Super-Sirius SQL coverage (Catch2 integration suite via transparent interception + kit A/B result parity; waiver evidence in `scratchpad/pr1/sqllogic-waiver.md`), and §10's per-pathway SQL tests are re-homed from extending `test/sql/` to the `test_gpu_execution_*` Catch2 integration pattern, §9/§10. All other PR-1 deviations from this doc: none found (P0 detection verbatim under rename per working-tree diff; §4.2 spec/concept/bundle realized as written; §6's host dispatch realized as a monomorphized switch — the one-bundle degenerate of the whitelist table).
- [mechanism/minor, PR-3 audit] §4.7's value-aggregate catalog identity as a static signature table is unimplementable: the binder rewrites min/max/avg/sum entries per argument type (BindMinMax specialization; DECIMAL avg/sum retargeting with scale bind data) -> amended: re-bind-and-compare authentication (`exact_builtin_value_aggregate_id` reproduces the binder's own bind of the system entry and requires exact callback/signature/flag/bind-data equality; sum_no_overflow compared against its catalog member and admitted as SUM); every mismatch, ambiguity, or throw is a miss — fail-closed in the same direction as the specified table, §4.7.
- [detection/minor, PR-3 audit] §5.1's verbatim shape missed q17's real optimized plans -> amended with four fail-closed widenings: composed bare-projection remap (RemoveUnusedColumns leaves column-selection projections at SF100+; computing/ill-typed hops miss), empty-grouping-set admission (subquery flattening leaves `grouping_sets` empty), value-safe group re-homing onto the qualifying side's key (sound under one plain COMPARE_EQUAL on identical unparameterized INTEGER/BIGINT types), and a declared-output-type screen (the operator's spec validation throws; a planner throw would fall the query to CPU rather than to generic GPU planning), §5.1. Hostile variants of each widening are unit-tested negatives; knob-off planning verified bit-identical (22/22 plan parity vs the archived PR-1 baseline).
- [scope/minor, PR-3 audit] §9 assigned all per-form narrowing/compressed-schema arms to PR-4, but the two-child value-sensitive arms (SUM/MIN/MAX/AVG args restore native, §4.9) are a correctness prerequisite for PR-3's value fusion -> amended: re-sliced — two-child value arms in PR-3, single-child DIRECT arms stay in PR-4, §9.
- [config/minor, PR-3 audit] §4.8's counted-byte gate had no knob row and §10's byte-gate negatives need a test hook -> amended: engine-owned `group_join_counted_bytes_gate` added to §4.7's table (smallest visible device's memory / 24; 0 = decline everything when no device is visible; YAML key rejected, internal SET hook rejecting 0 — the `group_join_max_state_bytes` pattern); the one-new-public-knob claim is unchanged, §4.7.
- [mechanism/minor, PR-3 audit] §4.9's publication row specified the runtime hook reuse but left the planning side unnamed -> amended: planning extracted as the shared `plan_single_key_membership_publication` helper reusing the hash join's evidence/admission/discovery/placement mechanisms with probe=counted/build=preserved orientation; counted-side build evidence declines fusion (no-downgrade rule); runtime realized as the hash join's one-shot CAS machine triggered from the first hint poll after the preserved producer finishes, whole-single-batch GPU-resident deliveries only (a FULL one-shot port has no later delivery, so a non-resident or multi-batch preserved side closes the window permanently); named follow-up: re-home `plan_comparison_join`'s inline discovery loop onto the helper, §4.9. All other PR-3 deviations from this doc: none found (P0 verbatim under the ladder inversion; wiring class 2, byte-gate derivation, publication-downgrade decline, and the default flip realized as written; the −10…25% q17 claim correctly not made — PR-3's evidence shows SF100 fuse-neutral, SF1000 decline-by-design, microbenchmark archived with its PR-5 implication stated).
- [estimate/minor, PR-4 audit] §5.2 expected q2's fused sparse task at every SF; delivered: q2 fuses (and runs sparse, as specified) at SF100 but **does not fuse at SF1000** — the plan-time byte gate declines because DuckDB's `EstimateCardinality` for the aggregate's child join reports the full ~800 M-row partsupp cardinality (19.2 GB > device/24 = 11.17 GB) without crediting the delim semi-filter that cuts the true input to ~640 K rows -> accepted as the design's own honest-refusal class (§4.8: "plan-time estimates do not see dynamic filters", stated for q17; the same fact governs q2's child estimate); §9's "accept neutral" gate met via SF1000 22/22 plan/result parity; PR-5's `BUILD_STREAM` removes the gate and with it this decline, §5.2/§4.8/§9.
- [mechanism/minor, PR-4 audit] §4.8's wiring class 3 named the "standard single-child sink pattern (base-class shape)" without stating how the child reaches it -> amended: reached by adding `GROUP_JOIN` to the base `is_sink()` parent list (alongside PARTITION / RIGHT_DELIM_JOIN — parents consuming across one-shot FULL ports); the change is knob-independent and was verified benign across every consumer (DELIM_SCAN early-returns before its `is_sink()` check, so class-2 wiring is unaffected; knob-off plans and results bit-identical vs PR-3). PR-5 commitment recorded: when the streamed counted port goes PIPELINE, this parent-type test must become barrier-aware at the same site, §4.8.
- [test/minor, PR-4 audit] §10's dense-forcing reachability bullet asserted `last_strategy() == DENSE`; the integration realization asserts the operator's strategy log line ("DIRECT MIN dense path", with the sparse line asserted absent) since the operator instance is not reachable through a connection -> accepted: the proof obligation — full planner, rung P2, dense strategy selected, NULL-group slot exercised, CPU-oracle-identical results — is discharged; `last_strategy()` remains the assertion channel in the operator-level Catch2 suites, §10. All other PR-4 deviations from this doc: none found (P0 rung verbatim; P1 body untouched with its screens extracted into the shared value-rung helpers §4.7 prescribed; DIRECT anti-overlap join-root guard, opaque-child planning, argument-validity-gate soundness for padding NULLs, per-form single-child narrowing/compressed-schema arms, fail-closed wiring in both arity directions, ctor rejection of DIRECT publication, and the PR-3 named follow-up (shared `discover_membership_publication_targets`) all realized as written).
