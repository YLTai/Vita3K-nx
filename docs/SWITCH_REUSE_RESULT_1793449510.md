# PCSD00035 capture 1793449510

Input: `tracelab-PCSD00035-1793449510-1.zip`, CI #73 / head `039da60d`, merge `a3d3267bcd4fc725470b64b8cf9c61a975dc8b09`.
Session: 217.767030 s, 4,821 consecutive render IDs (1–4821), reported overflows zero. Controls: 76 PASS / 7 SKIP / 0 FAIL.

| Stage | Samples | Measured seconds | Loop/s |
|---|---:|---:|---:|
| STOCK 0 | 55 | 4.023316 | 13.670315 |
| AUDIT 1 | 54 | 4.038056 | 13.372771 |
| REUSE 2 | 57 | 4.042183 | 14.101290 |
| REUSE 3 | 57 | 4.012059 | 14.207170 |
| AUDIT 4 | 53 | 4.034477 | 13.136771 |
| STOCK 5 | 54 | 4.015626 | 13.447469 |

Pooled rate = sum(samples) * 19,200,000 / sum(ticks). STOCK: 13.5589985731; REUSE: 14.1540316111.
Descriptive throughput +4.388473%, average loop time 73.751759 -> 70.651248 ms (-3.100511 ms).
STOCK end/start drift -1.630149%. Two measured stages per mode do not establish statistical significance or all-scene benefit.

Actual REUSE work: 42,289 skipped / 58,899 calls (71.799182%); 3,280,537,672 skipped / 4,559,327,432 logical bytes (71.952228%).
The two AUDIT stages compare 17,324 + 17,264 = 34,588 candidate hashes (2,733,269,296 logical bytes); zero recorded contradictions.
One in-Hash state change declined a baseline; no cache capacity misses, exceptions, global source-policy blockers or early termination.
18 sticky excluded physical spans; GPU-written inputs remain ineligible. No new live Guest RO.

Work counters include settling/palette/repeated inputs; phase timing excludes settling and AUDIT.
Audits bracket performance; they do not individually re-read every skipped decision.
Both timed modes include common instrumentation and the same original scene memo.
This closes the fixed-scene diagnostic loop, not universal writer/lifecycle correctness or uninstrumented-baseline performance.
