# Why accumulated Bloom publication host-stages at 4 GPUs (user question)

- PR #2302 (pool peer-access API) works: intra-switch grants (0<->1, 2<->3) succeed.
- Platform: cross-switch P2P unsupported (`nvidia-smi topo -p2p r`/`w` = NS for 0-2,0-3,1-2,1-3; GPUs under different PCIe host bridges 44:00.0 / 55:00.0). Grant returns UNSUPPORTED -> "Pool access unavailable" x8 at init.
- Policy: src/cuda/sirius_dynamic_bloom_filter.cu:1080 `direct = !force_host && !find_pair_without_pool_access(spaces)` over ALL partial spaces -> any one missing pair => whole publication HOST-staged.
- Evidence (60 publications per run):
  | GPUs | transport | mean/max ms after final contribution |
  | {0,1} | peer copies x60 | 5.8 / 14.1 |
  | {0,2} | HOST staging x60 | 11.3 / 25.1 |
  | {0,1,2,3} | HOST staging x60 | 20.1 / 44.8 |
- Fix idea: hierarchical publication: reduce inside P2P islands via peer copies, host-stage only inter-island leg.
- Minor: for_each_pair_without_pool_access re-calls try_grant_pool_access per pair per publication (no cache).
