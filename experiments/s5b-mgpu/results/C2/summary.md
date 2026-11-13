# C2 summary

Generated 2026-11-13T22:56:05+00:00.

**Overall: PASS** (CPU match, no fallback and logged settings as SET for every label; no weaker, missing or duplicate Key Outcome and no lost Zone Map in any diff)

## key_4gpu

- build `key` (2b3b1c69), 4 GPU(s), SF1, settings: `defaults`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): none
- logged settings that disagree with the label: none
- Key Outcome kinds: {'bloom': 1, 'hash_in_list': 38, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): no multi-GPU Bloom Filter publication logged
- hash joins with more than one partition: 64
- (informational: left out of the verdict)

## tip_4gpu

- build `tip` (b65efbe3), 4 GPU(s), SF1, settings: `defaults`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): none
- logged settings that disagree with the label: none
- Key Outcome kinds: {'hash_in_list': 39, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): no multi-GPU Bloom Filter publication logged
- hash joins with more than one partition: 64

## tip_df_off_4gpu

- build `tip` (b65efbe3), 4 GPU(s), SF1, settings: `enable_dynamic_filter=false`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): none
- logged settings that disagree with the label: none
- Key Outcome kinds: {}
- reasons: {}
- Bloom Filter transfer path (`bloom_finish`): no multi-GPU Bloom Filter publication logged
- hash joins with more than one partition: 64

## key_2gpu

- build `key` (2b3b1c69), 2 GPU(s), SF1, settings: `defaults`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): none
- logged settings that disagree with the label: none
- Key Outcome kinds: {'bloom': 1, 'hash_in_list': 38, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): no multi-GPU Bloom Filter publication logged
- hash joins with more than one partition: 64
- (informational: left out of the verdict)

## tip_2gpu

- build `tip` (b65efbe3), 2 GPU(s), SF1, settings: `defaults`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): none
- logged settings that disagree with the label: none
- Key Outcome kinds: {'hash_in_list': 39, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): no multi-GPU Bloom Filter publication logged
- hash joins with more than one partition: 64

## tip_df_off_2gpu

- build `tip` (b65efbe3), 2 GPU(s), SF1, settings: `enable_dynamic_filter=false`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): none
- logged settings that disagree with the label: none
- Key Outcome kinds: {}
- reasons: {}
- Bloom Filter transfer path (`bloom_finish`): no multi-GPU Bloom Filter publication logged
- hash joins with more than one partition: 64

## Key Outcomes key_4gpu -> tip_4gpu

- statuses: {'equal': 52, 'stronger': 1}; Zone Maps lost: 0
- rows failing the strict bar: 0, of which explained by a declared delta (for the user to judge): 0 (see `diff_key_4gpu_vs_tip_4gpu.txt`)

## Key Outcomes key_2gpu -> tip_2gpu

- statuses: {'equal': 52, 'stronger': 1}; Zone Maps lost: 0
- rows failing the strict bar: 0, of which explained by a declared delta (for the user to judge): 0 (see `diff_key_2gpu_vs_tip_2gpu.txt`)

