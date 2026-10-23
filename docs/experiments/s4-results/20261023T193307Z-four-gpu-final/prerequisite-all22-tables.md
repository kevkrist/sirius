Each row concerns one query and one revision comparison. Ratios are final/baseline. The 95% interval is across independent process-pair mean log ratios; baseline/baseline drift is separate. Unsupported publication counters remain null.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Individual guard | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q1 | 5 | 30 | 179.057637 | 181.907953 | 188.438635 | 186.877684 | 1.00224 | 1.06117 | [0.97038, 1.04182] | 1.05121 | [0.98378, 1.08199] | regression_guard_inconclusive | {} |
| sql-q2 | 5 | 30 | 42.988987 | 43.513495 | 45.047960 | 45.282885 | 1.01183 | 1.06418 | [0.98232, 1.03571] | 0.98517 | [0.94438, 1.00924] | regression_guard_inconclusive | {} |
| sql-q3 | 5 | 30 | 72.630510 | 72.037872 | 75.750384 | 76.884325 | 0.99255 | 1.06570 | [0.95537, 1.03848] | 0.99551 | [0.98041, 1.04342] | regression_guard_inconclusive | {} |
| sql-q4 | 5 | 30 | 58.716544 | 58.838885 | 62.023504 | 61.291827 | 0.99507 | 1.06004 | [0.96570, 1.02831] | 0.97461 | [0.93383, 1.02857] | regression_guard_pass | {} |
| sql-q5 | 5 | 30 | 59.879635 | 59.739543 | 62.268088 | 63.642304 | 0.99951 | 1.09424 | [0.97681, 1.03902] | 0.99917 | [0.97418, 1.01821] | regression_guard_inconclusive | {} |
| sql-q6 | 5 | 30 | 40.881115 | 41.048954 | 42.576554 | 43.771645 | 1.02728 | 1.11154 | [0.97637, 1.06734] | 0.99903 | [0.95275, 1.06380] | regression_guard_inconclusive | {} |
| sql-q7 | 5 | 29 | 70.476443 | 71.236982 | 72.764082 | 74.965313 | 1.01255 | 1.08960 | [0.96518, 1.06500] | 0.99186 | [0.97099, 1.01039] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1} |
| sql-q8 | 5 | 30 | 79.314545 | 76.473767 | 89.005057 | 80.699616 | 0.95251 | 1.03997 | [0.93200, 0.97700] | 0.97609 | [0.95108, 1.01550] | baseline_control_missing | {} |
| sql-q9 | 5 | 17 | 160.077206 | 161.626739 | 168.292285 | 168.692177 | 0.99332 | 1.07293 | [0.96891, 1.03217] | 1.01580 | [0.97945, 1.04156] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 13} |
| sql-q10 | 5 | 30 | 149.161842 | 150.560037 | 153.918426 | 153.936038 | 1.00826 | 1.04164 | [0.99246, 1.01773] | 1.00058 | [0.98071, 1.02328] | regression_guard_pass | {} |
| sql-q11 | 5 | 30 | 27.082148 | 26.833424 | 28.100599 | 27.974794 | 1.00420 | 1.07125 | [0.96346, 1.02938] | 0.97267 | [0.94979, 0.99129] | inconclusive_baseline_control_drift | {} |
| sql-q12 | 5 | 30 | 60.846136 | 61.459991 | 64.852867 | 65.263229 | 1.01855 | 1.10000 | [0.98307, 1.03173] | 0.99388 | [0.94495, 1.04763] | regression_guard_inconclusive | {} |
| sql-q13 | 5 | 30 | 42.442540 | 43.099245 | 44.111782 | 45.316969 | 1.02043 | 1.09537 | [0.98311, 1.05886] | 0.99785 | [0.99800, 1.02029] | regression_guard_inconclusive | {} |
| sql-q14 | 5 | 30 | 62.732569 | 61.764870 | 66.337299 | 63.025046 | 0.97024 | 1.01869 | [0.94486, 0.99579] | 0.98737 | [0.96077, 1.02566] | regression_guard_pass | {} |
| sql-q15 | 5 | 30 | 65.041758 | 66.374076 | 69.679425 | 69.205482 | 1.00854 | 1.10064 | [0.94890, 1.08148] | 0.99948 | [0.98274, 1.03169] | regression_guard_inconclusive | {} |
| sql-q16 | 5 | 30 | 63.376195 | 63.503237 | 64.504211 | 64.104963 | 1.00301 | 1.02303 | [0.99662, 1.00761] | 0.99990 | [0.98864, 1.01087] | regression_guard_pass | {} |
| sql-q17 | 5 | 30 | 33.984461 | 34.795876 | 35.919853 | 36.634687 | 1.02087 | 1.06893 | [0.99424, 1.04622] | 0.99082 | [0.94741, 1.02597] | regression_guard_inconclusive | {} |
| sql-q18 | 5 | 30 | 212.648591 | 214.344733 | 218.585040 | 221.999639 | 1.01629 | 1.05323 | [0.99232, 1.03127] | 1.01063 | [0.98268, 1.01325] | regression_guard_inconclusive | {} |
| sql-q19 | 5 | 30 | 85.871539 | 85.524915 | 88.992795 | 89.012480 | 0.98994 | 1.04998 | [0.94496, 1.03999] | 1.01823 | [0.96317, 1.06430] | regression_guard_inconclusive | {} |
| sql-q20 | 5 | 30 | 47.757329 | 47.818975 | 50.506862 | 50.187352 | 1.00440 | 1.06815 | [0.98640, 1.02179] | 1.00787 | [0.98582, 1.03602] | regression_guard_pass | {} |
| sql-q21 | 5 | 30 | 243.262473 | 245.053731 | 247.909115 | 253.421453 | 1.01966 | 1.05797 | [0.98723, 1.03767] | 0.99528 | [0.97408, 1.02490] | regression_guard_inconclusive | {} |
| sql-q22 | 5 | 30 | 47.411917 | 47.745145 | 48.534735 | 49.236424 | 1.01039 | 1.04942 | [0.98933, 1.02861] | 1.00339 | [0.97923, 1.02582] | regression_guard_pass | {} |

Publication fields for baseline/final arms (distinct per-query values; contributing-GPU counts accumulate across publications):

| Query | Arm | Publication count | Metrics supported | Published bytes | Input bytes | Ready bytes | Admitted keys | GPU participation count | Publication median ms | Skip reason |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q1 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q1 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q2 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q2 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q3 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q3 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q4 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q4 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q5 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q5 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q6 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q6 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q7 | baseline | 1 | false | null | null | null | null | null | unavailable | null |
| sql-q7 | candidate | 1 | true | 23982720 | 95930880 | 95930880 | 1 | 4 | 3.402358 | null |
| sql-q8 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q8 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q9 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q9 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q10 | baseline | 1 | false | null | null | null | null | null | unavailable | null |
| sql-q10 | candidate | 1 | true | 11474848 | 45899392 | 45899392 | 1 | 4 | 1.843553 | null |
| sql-q11 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q11 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q12 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q12 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q13 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q13 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q14 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q14 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q15 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q15 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q16 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q16 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q17 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q17 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q18 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q18 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q19 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q19 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q20 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q20 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q21 | baseline | 3 | false | null | null | null | null | null | unavailable | null |
| sql-q21 | candidate | 3 | true | 174974624 | 699898496 | 699898496 | 3 | 12 | 24.542715 | null |
| sql-q22 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q22 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |

Publication interval comparisons are supported only when both revisions expose preparation timing and equivalent publication occurred. Prerequisite fields are unsupported, and Q4 has no publication observation. These rows cannot replace the end-to-end query guard.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Assessment | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q1 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q2 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q3 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q4 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q5 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q6 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q7 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1, 'publication_work_not_equivalent_or_not_published': 29} |
| sql-q8 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q9 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 13, 'publication_work_not_equivalent_or_not_published': 17} |
| sql-q10 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q11 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q12 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q13 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q14 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q15 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q16 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q17 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q18 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q19 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q20 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q21 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q22 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
