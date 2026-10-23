set(SIRIUS_S4_PUBLICATION_SCHEDULE
    "current_direct"
    CACHE STRING "Private S4 experiment: current_direct or concurrent_ingress")
set_property(CACHE SIRIUS_S4_PUBLICATION_SCHEDULE
             PROPERTY STRINGS current_direct concurrent_ingress)
if(SIRIUS_S4_PUBLICATION_SCHEDULE STREQUAL "concurrent_ingress")
  set_property(
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/sirius_dynamic_bloom_filter.cu"
           TARGET_DIRECTORY sirius_objects
    APPEND
    PROPERTY COMPILE_DEFINITIONS SIRIUS_S4_CONCURRENT_INGRESS=1)
elseif(NOT SIRIUS_S4_PUBLICATION_SCHEDULE STREQUAL "current_direct")
  message(
    FATAL_ERROR
      "Unknown S4 publication schedule: ${SIRIUS_S4_PUBLICATION_SCHEDULE}")
endif()

target_sources(
  sirius_objects
  PRIVATE date_cast.cu
          dense_count_join_impl.cu
          device_copy_batch.cpp
          dynamic_filter_mask_ops.cu
          dynamic_filter_replica_transfer.cu
          sirius_dynamic_bloom_filter.cu
          sirius_dynamic_in_list_filter.cu
          sirius_dynamic_small_in_list_filter.cu
          sirius_like_multiliteral.cu
          sirius_round_floating_point.cu
          scan/gpu_decode_alp.cu
          scan/gpu_decode_bitpacking.cu
          scan/gpu_decode_rle.cu
          scan/gpu_decode_strings.cu
          scan/gpu_native_decode.cu
          scan/strings/dict_fsst.cu
          scan/strings/dictionary.cu
          scan/strings/fsst.cu
          scan/strings/uncompressed.cu
          vss/cudf_raft_interop.cu
          vss/brute_force_search.cu
          vss/ivf_flat_index.cu)
