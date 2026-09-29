# Review of predeclared system-memory guard findings

Both runs passed the supervisor-RSS growth budget. The system-available guard
flagged baseline -4.11 GiB and candidate -11.60 GiB, with different starting
availability (19.27 vs 24.20 GiB). These runs did not control system cache,
compression or unrelated application allocation; baseline also overlapped scoped
build/contract commands. They cannot substantiate a general Metal-load speedup.
The unchanged daemon hash and unchanged initialization algorithm support that
restriction. Only the explicit active-model no-op is attributed to this change.

All 66 baseline and 56 candidate owned process IDs were checked again after the
runs: none survived. Every replacement also checked exit/reap before spawn during
the test; the supervisor RSS ended at 7.66 / 9.48 MiB. No retained model process or
unbounded supervisor growth was observed in these bounded runs.

The later OS snapshot is contextual, not a reconstructed baseline: file-backed
pages, approximately 27 GiB of compressor occupancy, and 7.81 MiB swap usage were
present. The owner's known app/service/model processes remained alive and idle;
none was stopped. Without before/after driver and compressor attribution, the
system-wide delta cannot be assigned to a Geist allocation leak or declared
reclaimed. Raw diagnostics: 39-memory-review-after.json.

Disposition: retain the system-context flags and do not claim unique physical
memory recovery or general Metal load acceleration. The #40 owner-device telemetry
and repeated backend allocation/release tests provide the next scoped evidence.
The process-lifecycle improvement is accepted on ownership/reap, bounded parent
RSS and unchanged inference behavior, not exact free-RAM restoration.
