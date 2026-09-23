# Optimized dynamic-filter reference

Full integrated prototype based on `c560b286`, separate from current `dev` and S1/S2.

## Get the required cuCascade checkout

```bash
git clone --branch reference/df-optimized-20260917 \
  https://github.com/kevkrist/sirius.git
cd sirius
git submodule update --init --recursive -- duckdb substrait vcpkg cucascade
git -C cucascade rev-parse HEAD
```

cuCascade must report `31155d66a8bd58b21a8edc06939ca9d7f77d978c`.
The submodule URL points to [the matching fork branch](https://github.com/kevkrist/cuCascade/tree/reference/df-optimized-20260917),
and the gitlink pins this exact revision. Do not substitute cuCascade `main` or use
`git submodule update --remote`; no separate patch is needed.

## Enable all optimizations

[dynamic-filter-all-on.yaml](dynamic-filter-all-on.yaml) explicitly enables all six
optimizations and multi-partition filters. Merge its settings into your own Sirius
configuration, keeping your hardware, memory and workload settings, then point
Sirius at that file before starting your process:

```bash
export SIRIUS_CONFIG_FILE=/absolute/path/to/your-sirius.yaml
```

The scan-view and pinned-domain optimizations require GPU-pinned data. For Parquet
pins, supply valid unique-key declarations through `pin_table(..., unique_cols=[...])`
so the domain-coverage optimization has the evidence it needs. Pool peer access and
pipelined publication also require supported GPU connectivity; enabled settings
do not override capability checks.

The retained older-driver guard requires no setup: on a compatible newer driver
it selects the original CUDA calls, not the fallback.
