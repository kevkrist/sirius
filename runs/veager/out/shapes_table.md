| shape | tip s (hot) | f1: result = tip / s (hot) / status | fk1: result = tip / s (hot) / status | s2g1: result = tip / s (hot) / status | s2g0: result = tip / s (hot) / status | cpu: result = tip / s (hot) / status |
|---|---|---|---|---|---|---|
| nofilter_plan | 0.756 | same / 0.559 / ok,ok | same / 0.526 / ok,ok | same / 0.554 / ok,ok | same / 0.556 / ok,ok | same / 2.574 / ok |
| declined_domain | 0.693 | same / 0.638 / ok,ok | same / 0.654 / ok,ok | same / 0.651 / ok,ok | same / 0.651 / ok,ok | same / 2.777 / ok |
| selective | 0.357 | same / 0.319 / ok,ok | same / 0.371 / ok,ok | same / 0.378 / ok,ok | same / 0.295 / ok,ok | same / 0.88 / ok |
| empty_build | 0.178 | same / 0.175 / ok,ok | same / 0.172 / ok,ok | same / 0.174 / ok,ok | same / 0.178 / ok,ok | same / 0.097 / ok |
| empty_build_left | 0.014 | same / 0.014 / ok,ok | same / 0.014 / ok,ok | same / 0.014 / ok,ok | same / 0.014 / ok,ok | same / 0.048 / ok |
| one_row_build | 0.207 | same / 0.191 / ok,ok | same / 0.189 / ok,ok | same / 0.242 / ok,ok | same / 0.19 / ok,ok | same / 0.25 / ok |
| empty_probe | 0.073 | same / 0.055 / ok,ok | same / 0.076 / ok,ok | same / 0.061 / ok,ok | same / 0.059 / ok,ok | same / 0.103 / ok |
| multi_filter_scan | 0.143 | same / 0.141 / ok,ok | same / 0.141 / ok,ok | same / 0.143 / ok,ok | same / 0.141 / ok,ok | same / 0.443 / ok |
| multi_filter_3 | 0.209 | same / 0.199 / ok,ok | same / 0.214 / ok,ok | same / 0.202 / ok,ok | same / 0.204 / ok,ok | same / 0.218 / ok |
| stacked4 | 0.398 | same / 0.318 / ok,ok | same / 0.307 / ok,ok | same / 0.317 / ok,ok | same / 0.306 / ok,ok | same / 0.679 / ok |
| self_join | 0.06 | same / 0.06 / ok,ok | same / 0.059 / ok,ok | same / 0.058 / ok,ok | same / 0.059 / ok,ok | same / 0.068 / ok |
| self_join_orders | 0.324 | same / 0.298 / ok,ok | same / 0.35 / ok,ok | same / 0.275 / ok,ok | same / 0.293 / ok,ok | same / 0.384 / ok |
| limit10 | 0.324 | same / 0.322 / ok,ok | same / 0.309 / ok,ok | same / 0.322 / ok,ok | same / 0.319 / ok,ok | same / 0.146 / ok |
| limit0 | 0.005 | same / 0.005 / ok,ok | same / 0.005 / ok,ok | same / 0.004 / ok,ok | same / 0.004 / ok,ok | same / 0.004 / ok |
| limit_plain | 0.326 | same / 0.272 / ok,ok | same / 0.269 / ok,ok | same / 0.278 / ok,ok | same / 0.277 / ok,ok | same / 0.162 / ok |
| limit_multi | 0.138 | same / 0.135 / ok,ok | same / 0.14 / ok,ok | same / 0.136 / ok,ok | same / 0.137 / ok,ok | same / 0.278 / ok |
| semi_in | 0.279 | same / 0.222 / ok,ok | same / 0.276 / ok,ok | same / 0.24 / ok,ok | same / 0.22 / ok,ok | same / 0.492 / ok |
| anti_notexists | 0.071 | same / 0.071 / ok,ok | same / 0.071 / ok,ok | same / 0.072 / ok,ok | same / 0.073 / ok,ok | same / 0.079 / ok |
| mark_or | 0.047 | same / 0.047 / ok,ok | same / 0.048 / ok,ok | same / 0.047 / ok,ok | same / 0.048 / ok,ok | same / 0.058 / ok |
| left_join | 0.319 | same / 0.32 / ok,ok | same / 0.311 / ok,ok | same / 0.331 / ok,ok | same / 0.307 / ok,ok | same / 0.518 / ok |
| right_join | 0.265 | same / 0.296 / ok,ok | same / 0.29 / ok,ok | same / 0.286 / ok,ok | same / 0.29 / ok,ok | same / 0.38 / ok |
| full_join | 0.085 | same / 0.092 / ok,ok | same / 0.088 / ok,ok | same / 0.083 / ok,ok | same / 0.084 / ok,ok | same / 0.079 / ok |
| target_in_lower_build | 0.47 | same / 0.431 / ok,ok | same / 0.409 / ok,ok | same / 0.418 / ok,ok | same / 0.409 / ok,ok | same / 1.367 / ok |
| union_probe | 0.624 | same / 0.545 / ok,ok | same / 0.535 / ok,ok | same / 0.52 / ok,ok | same / 0.589 / ok,ok | same / 0.645 / ok |
| cte_shared | 0.339 | same / 0.327 / ok,ok | same / 0.342 / ok,ok | same / 0.342 / ok,ok | same / 0.307 / ok,ok | same / 0.374 / ok |
| bushy | 0.446 | same / 0.404 / ok,ok | same / 0.455 / ok,ok | same / 0.438 / ok,ok | same / 0.459 / ok,ok | same / 0.46 / ok |
| ineq_residual | 0.412 | same / 0.393 / ok,ok | same / 0.403 / ok,ok | same / 0.392 / ok,ok | same / 0.37 / ok,ok | same / 0.642 / ok |
| agg_build | 0.207 | same / 0.218 / ok,ok | same / 0.219 / ok,ok | same / 0.216 / ok,ok | same / 0.221 / ok,ok | same / 0.169 / ok |
| corr_scalar_delim | 0.115 | same / 0.108 / ok,ok | same / 0.114 / ok,ok | same / 0.112 / ok,ok | same / 0.104 / ok,ok | same / 0.176 / ok |
| corr_exists_ineq | 0.071 | same / 0.069 / ok,ok | same / 0.071 / ok,ok | same / 0.071 / ok,ok | same / 0.069 / ok,ok | same / 0.495 / ok |
| corr_agg_join | 1.51 | same / 1.3 / ok,ok | same / 1.3 / ok,ok | same / 1.423 / ok,ok | same / 1.29 / ok,ok | same / 6.236 / ok |
| star4 | 0.283 | same / 0.215 / ok,ok | same / 0.228 / ok,ok | same / 0.246 / ok,ok | same / 0.26 / ok,ok | same / 0.422 / ok |
| topn_join | 0.315 | same / 0.291 / ok,ok | same / 0.292 / ok,ok | same / 0.279 / ok,ok | same / 0.3 / ok,ok | same / 0.48 / ok |
| window_probe | 0.355 | same / 0.331 / ok,ok | same / 0.304 / ok,ok | same / 0.348 / ok,ok | same / 0.337 / ok,ok | same / 0.67 / ok |
| two_keys | 0.183 | same / 0.191 / ok,ok | same / 0.188 / ok,ok | same / 0.192 / ok,ok | same / 0.191 / ok,ok | same / 0.446 / ok |
| null_keys_left_chain | 0.067 | same / 0.065 / ok,ok | same / 0.066 / ok,ok | same / 0.065 / ok,ok | same / 0.068 / ok,ok | same / 0.058 / ok |
| string_key | 0.192 | same / 0.19 / ok,ok | same / 0.199 / ok,ok | same / 0.181 / ok,ok | same / 0.182 / ok,ok | same / 0.159 / ok |
f1: interrupts {'interrupted': 13, 'finished_before_interrupt': 2}; queries after an interrupt correct 16/16 (s: [0.318, 0.636, 0.335, 0.368, 0.484, 0.291, 0.286, 0.574, 0.335, 0.372, 0.481, 0.333, 0.307, 0.481, 0.308, 0.365]); HANG records []; closed True
fk1: interrupts {'interrupted': 13, 'finished_before_interrupt': 2}; queries after an interrupt correct 16/16 (s: [0.365, 0.568, 0.298, 0.355, 0.488, 0.332, 0.318, 0.488, 0.334, 0.304, 0.488, 0.3, 0.356, 0.48, 0.344, 0.357]); HANG records []; closed True
s2g1: interrupts {'interrupted': 13, 'finished_before_interrupt': 2}; queries after an interrupt correct 16/16 (s: [0.308, 0.485, 0.333, 0.279, 0.641, 0.299, 0.316, 0.48, 0.298, 0.318, 0.483, 0.314, 0.321, 0.631, 0.313, 0.285]); HANG records []; closed True
s2g0: interrupts {'interrupted': 13, 'finished_before_interrupt': 2}; queries after an interrupt correct 16/16 (s: [0.321, 0.632, 0.341, 0.321, 0.486, 0.319, 0.355, 0.566, 0.307, 0.295, 0.479, 0.352, 0.358, 0.483, 0.296, 0.355]); HANG records []; closed True
cpu: interrupts {}; queries after an interrupt correct 0/0 (s: []); HANG records []; closed True
