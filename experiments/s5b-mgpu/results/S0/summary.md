# S0 summary

Generated 2026-11-13T21:52:53+00:00.

## Builds
```
2026-11-13T21:27:14+00:00 build=tip commit=b65efbe35c83fcb14af0b6945731a7c35f8b41e5 ok=yes seconds=282
2026-11-13T21:35:03+00:00 build=key commit=2b3b1c69b6ebfc3d93a830b27686678f11f00a85 ok=yes seconds=469
2026-11-13T21:41:05+00:00 build=base commit=fec547192ea588e08aee1de08242ecf9443740e0 ok=yes seconds=362
```

## Data
```
SF1: ok: /opt/dlami/nvme/s5b-mgpu/data/tpch_parquet_sf1 0.2 GiB (0.5 GiB uncompressed), 8 files, lineitem rows 6001215
SF10: ok: /opt/dlami/nvme/s5b-mgpu/data/tpch_parquet_sf10 2.5 GiB (4.6 GiB uncompressed), 8 files, lineitem rows 59986052
SF100: ok: /opt/dlami/nvme/s5b-mgpu/data/tpch_parquet_sf100 25.8 GiB (48.8 GiB uncompressed), 14 files, lineitem rows 600037902
SF1000: ok: /opt/dlami/nvme/s5b-mgpu/data/tpch_parquet_sf1000 264.5 GiB (493.8 GiB uncompressed), 90 files, lineitem rows 5999989709
```

## Configs (4 GPUs: the machine config unchanged; 1 and 2 GPUs: only topology.num_gpus changed)
```
  sirius.topology.num_gpus: 4
  sirius.memory.gpu.usage_limit_fraction: 0.9
  sirius.memory.gpu.usage_limit_bytes: (default)
  sirius.memory.host.capacity_fraction: (default)
  sirius.memory.host.capacity_bytes: 600GB
  sirius.memory.disk.downgrade_root_dirs: (default)
  sirius.executor.scan_manager.cache.mode: (default)
  sirius.compression.enable_pin_table_compression: True
  sirius.compression.input_plan_dir: /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000
  sirius.telemetry.enable_quent: True
  sirius.telemetry.output_directory: /home/ubuntu/sirius/results/telemetry
[21:52:54] configs ok; copies in /opt/dlami/nvme/s5b-mgpu/control/experiments/s5b-mgpu/results/S0/configs
```

## Resolved settings (from probe; see settings.parquet)
```
base_1gpu: {"concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
base_4gpu: {"concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0,1,2,3","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
key_2gpu: {"concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0,1","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
key_4gpu: {"concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0,1,2,3","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
tip_1gpu: {"build_fold_policy":"legacy_filter_coupled","concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
tip_2gpu: {"build_fold_policy":"legacy_filter_coupled","concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0,1","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
tip_4gpu: {"build_fold_policy":"legacy_filter_coupled","concat_batch_bytes":"1000000000","enable_dynamic_filter":"true","enable_dynamic_zone_map_filter":"false","fallback":"False","gpu_completed":"1","gpus_seen":"0,1,2,3","hash_partition_bytes":"1000000000","lineitem_rows_sf1":"6001215","max_broadcast_join_size":"3000000000","max_build_hash_table_bytes":"8000000000","max_build_probe_resident_bytes":"18355653176","max_dynamic_filter_bloom_bytes_per_gpu":"268435456","pin_compressed":"False","pinned_scans":"1","scan_task_batch_size":"4000000000"}
```

## P2P (from hardware.md)
```
```
 	[4mGPU0	GPU1	GPU2	GPU3	[0m
 GPU0	X	OK	NS	NS	
 GPU1	OK	X	NS	NS	
 GPU2	NS	NS	X	OK	
 GPU3	NS	NS	OK	X	

Legend:

  X    = Self
  OK   = Status Ok
  CNS  = Chipset not supported
  GNS  = GPU not supported
  TNS  = Topology not supported
  NS   = Not supported
  U    = Unknown
 	[4mGPU0	GPU1	GPU2	GPU3	[0m
 GPU0	X	OK	NS	NS	
 GPU1	OK	X	NS	NS	
 GPU2	NS	NS	X	OK	
 GPU3	NS	NS	OK	X	

Legend:

  X    = Self
  OK   = Status Ok
  CNS  = Chipset not supported
  GNS  = GPU not supported
  TNS  = Topology not supported
  NS   = Not supported
  U    = Unknown
```

```
