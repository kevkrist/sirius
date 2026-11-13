# C5 summary

Generated 2026-11-13T23:11:46+00:00.

**Overall: PASS** (CPU match, no fallback and logged settings as SET for every label; no weaker, missing or duplicate Key Outcome and no lost Zone Map in any diff; no OOM retries above `never`)

## never

- build `tip` (b65efbe3), 4 GPU(s), SF10, settings: `max_build_hash_table_bytes=0 concat_batch_bytes=125000000 build_fold_policy='never'`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): ['q11']
- logged settings that disagree with the label: none
- Key Outcome kinds: {'bloom': 1, 'hash_in_list': 38, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): {'host_staging': 1}
- hash joins with more than one partition: 1

## legacy_filter_coupled

- build `tip` (b65efbe3), 4 GPU(s), SF10, settings: `max_build_hash_table_bytes=0 concat_batch_bytes=125000000 build_fold_policy='legacy_filter_coupled'`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): ['q11']
- logged settings that disagree with the label: none
- Key Outcome kinds: {'bloom': 1, 'hash_in_list': 38, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): {'host_staging': 1}
- hash joins with more than one partition: 1

## bounded

- build `tip` (b65efbe3), 4 GPU(s), SF10, settings: `max_build_hash_table_bytes=0 concat_batch_bytes=125000000 build_fold_policy='bounded'`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): ['q11']
- logged settings that disagree with the label: none
- Key Outcome kinds: {'bloom': 1, 'hash_in_list': 38, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): {'host_staging': 1}
- hash joins with more than one partition: 1

## Resources against `never`

- OOM retries above `never` (fails the bar): 0
- downgrade bytes above `never` (reported, not a failure): 0

