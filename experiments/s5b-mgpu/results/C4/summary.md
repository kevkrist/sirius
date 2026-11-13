# C4 summary

Generated 2026-11-13T23:02:01+00:00.

**Overall: PASS** (CPU match, no fallback and logged settings as SET for every label; no weaker, missing or duplicate Key Outcome and no lost Zone Map in any diff)

## tip_4gpu

- build `tip` (b65efbe3), 4 GPU(s), SF10, settings: `defaults`
- CPU match: PASS; mismatches none; GPU fallback none; not run none
- equal to the CPU only as multisets (row order differs; check ORDER BY ties): ['q11']
- logged settings that disagree with the label: none
- Key Outcome kinds: {'bloom': 1, 'hash_in_list': 38, 'small_in_list': 14}
- reasons: {'-': 53}
- Bloom Filter transfer path (`bloom_finish`): {'host_staging': 1}
- hash joins with more than one partition: 64

