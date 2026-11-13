#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Stage S0: setup. Run with plain bash; every tool runs through `pixi run` inside.
#
#   bash experiments/s5b-mgpu/bin/s0_setup.sh <step> [args]
#
# Steps, in order (each is idempotent and can be rerun):
#   check              tools, remotes, 4 GPUs, nothing running, machine config, SF1000 path, disk
#   hardware           GPUs, PCIe topology, P2P support, host memory -> results/S0/hardware.md
#   fetch              fetch the three stack branches; verify the commits and the shared pixi.lock
#   worktrees          create the tip, key and base worktrees and their submodules
#   build [names...]   `pixi run make` in each worktree, one at a time (a step; 4 h limit)
#   selftest           host-only checks of the scripts and the parity script (after 'build')
#   data               check TPC-H Parquet SF1/10/100/1000 -> results/S0/data.parquet
#   configs            the machine's 4-GPU config, and 1/2-GPU copies -> results/S0/configs/
#   queries            write the 22 queries per scale factor
#   probe              load each build/config, read back H, smoke-test a disk and a pinned scan
#   summarize          write results/S0/summary.md
#   generate <1|10|100>  generate one missing dataset (ONLY after the S0 stop and the user's go)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

S0_RESULTS="$S5B_RESULTS/S0"
S0_WORK="$S5B_WORK/S0"

visible_gpus() { nvidia-smi --query-gpu=index --format=csv,noheader | wc -l; }

step_check() {
  local ok=1
  for tool in git pixi nvidia-smi du df timeout pgrep; do
    command -v "$tool" > /dev/null || { s5b_log "missing tool: $tool"; ok=0; }
  done
  local url
  url="$(git -C "$S5B_CONTROL" remote get-url "$S5B_UPSTREAM_REMOTE" 2> /dev/null || true)"
  [[ "$url" == *sirius-db/sirius* ]] || { s5b_log "remote $S5B_UPSTREAM_REMOTE is '$url', expected sirius-db/sirius"; ok=0; }
  url="$(git -C "$S5B_CONTROL" remote get-url "$S5B_FORK_REMOTE" 2> /dev/null || true)"
  [[ -n "$url" ]] || { s5b_log "remote $S5B_FORK_REMOTE (the user's fork) is missing"; ok=0; }
  local gpus
  gpus="$(visible_gpus)"
  s5b_log "GPUs: $gpus (the kit unsets CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-unset} for every Sirius process)"
  ((gpus == 4)) || { s5b_log "need exactly 4 GPUs, nvidia-smi shows $gpus"; ok=0; }
  [[ -n "$S5B_SIRIUS_CONFIG" && -f "$S5B_SIRIUS_CONFIG" ]] ||
    { s5b_log "S5B_SIRIUS_CONFIG ('$S5B_SIRIUS_CONFIG') is not a file: ask the user for the machine's 4-GPU Sirius config"; ok=0; }
  [[ -n "$S5B_TPCH_SF1000" && -d "$S5B_TPCH_SF1000" ]] ||
    { s5b_log "S5B_TPCH_SF1000 ('$S5B_TPCH_SF1000') is not a directory: ask the user where the SF1000 Parquet data is"; ok=0; }
  s5b_ensure_work
  (s5b_guard_idle) || ok=0
  mkdir -p "$S5B_HOME" "$S5B_DATA"
  df -h "$S5B_HOME" "$S5B_DATA" "$S5B_WORK" | sed 's/^/  /' >&2
  free -g | sed 's/^/  /' >&2
  s5b_log "S5B_HOME=$S5B_HOME S5B_DATA=$S5B_DATA S5B_WORK=$S5B_WORK"
  ((ok)) || s5b_die "check failed"
  s5b_log "check passed"
}

step_hardware() {
  mkdir -p "$S0_RESULTS"
  local out="$S0_RESULTS/hardware.md"
  {
    echo "# S0 hardware"
    echo
    echo "Recorded $(date -Is) on $(hostname -s)."
    echo
    echo '## GPUs'
    echo '```'
    nvidia-smi -L
    nvidia-smi --query-gpu=index,name,memory.total,pci.bus_id,pcie.link.gen.max,pcie.link.width.max,clocks.max.sm,persistence_mode,driver_version --format=csv
    nvidia-smi | sed -n 3,4p
    echo '```'
    echo
    echo '## PCIe topology (`nvidia-smi topo -m`)'
    echo '```'
    nvidia-smi topo -m
    echo '```'
    echo
    echo '## P2P read / write capability (`nvidia-smi topo -p2p r|w`)'
    echo '```'
    nvidia-smi topo -p2p r || echo "(topo -p2p r unsupported)"
    nvidia-smi topo -p2p w || echo "(topo -p2p w unsupported)"
    echo '```'
    echo
    echo '## Host'
    echo '```'
    uname -srm
    nproc
    free -g
    lscpu | grep -E 'Model name|Socket|NUMA node' || true
    if command -v numactl > /dev/null; then numactl -H | head -20; fi
    df -h "$S5B_DATA" "$S5B_WORK"
    echo '```'
  } > "$out" 2>&1
  s5b_log "wrote $out"
  sed -n '/P2P read/,/^## Host/p' "$out" >&2
}

step_fetch() {
  git -C "$S5B_CONTROL" fetch "$S5B_UPSTREAM_REMOTE" "${S5B_STACK_BRANCHES[@]}"
  local commit
  for build in "${S5B_BUILDS[@]}"; do
    commit="$(s5b_commit "$build")"
    git -C "$S5B_CONTROL" cat-file -e "${commit}^{commit}" || s5b_die "commit $commit ($build) not found after fetch"
    s5b_log "$build = $(git -C "$S5B_CONTROL" rev-parse "$commit") $(git -C "$S5B_CONTROL" log -1 --format=%s "$commit")"
  done
  git -C "$S5B_CONTROL" merge-base --is-ancestor "$S5B_TIP_COMMIT" "$S5B_UPSTREAM_REMOTE/stacked/df/05b-fold-rule" ||
    s5b_die "tip $S5B_TIP_COMMIT is not on $S5B_UPSTREAM_REMOTE/stacked/df/05b-fold-rule"
  # The scripts run every build's binaries in the tip worktree's environment; that is only sound
  # while the three commits share one pixi environment.
  git -C "$S5B_CONTROL" diff --quiet "$S5B_BASE_COMMIT" "$S5B_TIP_COMMIT" -- pixi.toml pixi.lock ||
    s5b_die "pixi.toml/pixi.lock differ between base and tip; stop and ask the user"
  s5b_log "fetch done; pixi environment identical across builds"
}

step_worktrees() {
  local wt commit
  for build in "${S5B_BUILDS[@]}"; do
    wt="$(s5b_worktree "$build")"
    commit="$(git -C "$S5B_CONTROL" rev-parse "$(s5b_commit "$build")")"
    if [[ ! -e "$wt" ]]; then
      git -C "$S5B_CONTROL" worktree add --detach "$wt" "$commit"
      s5b_record_created worktree "$wt"
    fi
    [[ "$(git -C "$wt" rev-parse HEAD)" == "$commit" ]] || s5b_die "$wt is not at $commit"
    [[ -z "$(git -C "$wt" status --porcelain --untracked-files=no)" ]] || s5b_die "$wt has local changes"
    git -C "$wt" submodule update --init --recursive
    s5b_log "$build worktree ready: $wt at ${commit:0:8}"
  done
}

step_build() {
  local names=("$@")
  ((${#names[@]})) || names=("${S5B_BUILDS[@]}")
  mkdir -p "$S0_RESULTS" "$S0_WORK"
  local wt log start ok
  for build in "${names[@]}"; do
    wt="$(s5b_worktree "$build")"
    log="$S0_WORK/build_$build.log"
    start=$(date +%s)
    s5b_log "building $build in $wt (log: $log)"
    if s5b_pixi_in "$wt" make > "$log" 2>&1; then ok=yes; else ok=no; fi
    local ext="$wt/sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension"
    [[ -f "$ext" && -x "$wt/sirius-duckdb/build/release/duckdb" ]] || ok=no
    printf '%s build=%s commit=%s ok=%s seconds=%s\n' "$(date -Is)" "$build" \
      "$(git -C "$wt" rev-parse HEAD)" "$ok" "$(($(date +%s) - start))" >> "$S0_RESULTS/builds.txt"
    if [[ "$ok" != yes ]]; then
      grep -nE 'error|Error|FAILED' "$log" | tail -40 > "$S0_RESULTS/build_${build}_errors.txt" || true
      s5b_die "build $build failed; excerpt in $S0_RESULTS/build_${build}_errors.txt"
    fi
    s5b_log "build $build ok in $(($(date +%s) - start)) s"
  done
}

step_selftest() {
  s5b_pixi_in "$S5B_WT_tip" python -I -B -m unittest discover -s "$S5B_DIR/lib" -p selftest.py
  s5b_pixi_in "$S5B_WT_tip" python -I -B -m unittest discover -s "$S5B_DIR/tools" -p 'test_dynamic_filter_key_parity.py'
}

step_data() { s5b_py "$S5B_DIR/lib/check_data.py" --sf 1,10,100,1000; }

step_generate() {
  local sf="${1:?usage: generate <1|10|100>}"
  [[ "$sf" == 1 || "$sf" == 10 || "$sf" == 100 ]] ||
    s5b_die "only SF1, SF10 and SF100 may be generated; SF1000 already exists (S5B_TPCH_SF1000)"
  local var="S5B_TPCH_SF$sf"
  local out="${!var:-$S5B_DATA/tpch_parquet_sf$sf}"
  [[ ! -e "$out" ]] || s5b_die "$out exists; the generator would skip it. Remove it only with the user's approval."
  local need_gb=$((sf * 3 / 10 + 1))
  local free_gb
  free_gb="$(df -BG --output=avail "$(dirname "$out")" | tail -1 | tr -dc '0-9')"
  ((free_gb > need_gb)) || s5b_die "need about ${need_gb} GB free at $(dirname "$out"), have ${free_gb} GB"
  mkdir -p "$S0_WORK"
  s5b_log "generating TPC-H SF$sf Parquet into $out (about ${need_gb} GB)"
  s5b_pixi_in "$S5B_WT_tip/test/tpch_performance" bash generate_tpch_data.sh "$sf" --format parquet --output "$out" \
    > "$S0_WORK/generate_sf$sf.log" 2>&1 || s5b_die "generation failed; see $S0_WORK/generate_sf$sf.log"
  s5b_log "generated $out"
}

step_configs() {
  [[ -n "$S5B_SIRIUS_CONFIG" && -f "$S5B_SIRIUS_CONFIG" ]] || s5b_die "S5B_SIRIUS_CONFIG is not set to a file; see 'check'"
  mkdir -p "$S5B_CONFIGS"
  cp "$S5B_SIRIUS_CONFIG" "$S5B_CONFIGS/sirius_4gpu.yaml"
  local n target
  for n in 1 2; do
    target="$S5B_CONFIGS/sirius_${n}gpu.yaml"
    cp "$S5B_SIRIUS_CONFIG" "$target"
    s5b_pixi_in "$S5B_WT_tip" python scripts/patch_config.py "$target" --opt "sirius.topology.num_gpus=$n" > /dev/null
    rm -f "$S5B_CONFIGS"/sirius_"${n}"gpu.*.bak
  done
  s5b_py "$S5B_DIR/lib/configs.py" --machine "$S5B_SIRIUS_CONFIG" --visible-gpus "$(visible_gpus)" --gpus 1,2
}

step_queries() { s5b_py "$S5B_DIR/lib/make_queries.py" --sf 1,10,100,1000; }
step_probe() { s5b_py "$S5B_DIR/lib/probe.py" "$@"; }

step_summarize() {
  local out="$S0_RESULTS/summary.md"
  mkdir -p "$S0_RESULTS"
  {
    echo "# S0 summary"
    echo
    echo "Generated $(date -Is)."
    echo
    echo "## Builds"
    echo '```'
    cat "$S0_RESULTS/builds.txt" 2> /dev/null || echo "(no builds recorded)"
    echo '```'
    echo
    echo "## Data"
    echo '```'
    s5b_py "$S5B_DIR/lib/check_data.py" --sf 1,10,100,1000 || true
    echo '```'
    echo
    echo "## Configs (4 GPUs: the machine config unchanged; 1 and 2 GPUs: only topology.num_gpus changed)"
    echo '```'
    s5b_py "$S5B_DIR/lib/configs.py" --machine "$S5B_SIRIUS_CONFIG" --visible-gpus "$(visible_gpus)" --gpus 1,2 2>&1 || true
    echo '```'
    echo
    echo "## Resolved settings (from probe; see settings.parquet)"
    echo '```'
    for f in "$S5B_WORK"/settings/*.json; do
      [[ -f "$f" ]] && echo "$(basename "$f" .json): $(tr -d '\n ' < "$f")"
    done
    echo '```'
    echo
    echo "## P2P (from hardware.md)"
    echo '```'
    sed -n '/P2P read/,/^## Host/p' "$S0_RESULTS/hardware.md" 2> /dev/null | sed '1d;$d'
    echo '```'
  } > "$out"
  s5b_log "wrote $out"
}

main() {
  local step="${1:?usage: s0_setup.sh <step> [args]; see the header}"
  shift
  case "$step" in
    build | generate) s5b_as_step "$(s5b_step_name S0 "$step" "$@")" 14400 "$0" "$step" "$@" ;;
    probe) s5b_as_step "$(s5b_step_name S0 "$step" "$@")" 3600 "$0" "$step" "$@" ;;
  esac
  case "$step" in
    check | hardware | fetch | worktrees | build | selftest | data | generate | queries | configs | probe | summarize)
      "step_$step" "$@"
      ;;
    *) s5b_die "unknown step '$step'" ;;
  esac
}
main "$@"
