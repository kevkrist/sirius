Each row concerns one query and one revision comparison. Ratios are final/baseline. The 95% interval is across independent process-pair mean log ratios; baseline/baseline drift is separate. Unsupported publication counters remain null.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Individual guard | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | 5 | 30 | 57.910591 | 57.341188 | 60.193656 | 60.303248 | 0.98863 | 1.10027 | [0.96208, 1.02979] | 1.00618 | [0.99793, 1.01850] | regression_guard_pass | {} |
| sql-q7 | 5 | 29 | 71.108113 | 71.216298 | 73.016453 | 73.291501 | 0.99388 | 1.03893 | [0.96746, 1.02203] | 0.98756 | [0.94125, 1.03738] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1} |
| sql-q21 | 5 | 30 | 239.220514 | 241.568385 | 247.227192 | 249.832658 | 1.00090 | 1.05785 | [1.00225, 1.02224] | 1.00831 | [0.97177, 1.03415] | regression_guard_pass | {} |

Publication fields for baseline/final arms (distinct per-query values; contributing-GPU counts accumulate across publications):

| Query | Arm | Publication count | Metrics supported | Published bytes | Input bytes | Ready bytes | Admitted keys | GPU participation count | Publication median ms | Skip reason |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | baseline | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q4 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q7 | baseline | 1 | true | 23982720 | 95930880 | 95930880 | 1 | 4 | 3.408461 | null |
| sql-q7 | candidate | 1 | true | 23982720 | 95930880 | 95930880 | 1 | 4 | 3.399666 | null |
| sql-q21 | baseline | 3 | true | 174974624 | 699898496 | 699898496 | 3 | 12 | 24.646927 | null |
| sql-q21 | candidate | 3 | true | 174974624 | 699898496 | 699898496 | 3 | 12 | 24.333917 | null |

Publication interval comparisons are supported only when both revisions expose preparation timing and equivalent publication occurred. Prerequisite fields are unsupported, and Q4 has no publication observation. These rows cannot replace the end-to-end query guard.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Assessment | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q7 | 5 | 29 | 3.410741 | 3.398601 | 3.436107 | 3.414099 | 0.99878 | 1.00596 | [0.99412, 0.99994] | 0.99932 | [0.99160, 1.00428] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1} |
| sql-q21 | 5 | 30 | 24.646927 | 24.333917 | 24.988751 | 24.777928 | 0.99605 | 1.04064 | [0.98060, 1.00149] | 1.00226 | [0.98453, 1.02955] | publication_timing_observation | {} |
