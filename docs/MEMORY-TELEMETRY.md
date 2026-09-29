# Scoped memory telemetry

The compact summary and Measurements sheet distinguish **Process RSS** from
**Metal allocated**. Neither is total model memory. Artifact/download bytes and
catalog working-memory estimates keep their own labels. Never add RSS and Metal
allocation: Apple Silicon shares physical memory, and these scopes overlap.

The optional public `geist_backend_resources_snapshot` API runs on the owning
daemon's backend. The Metal implementation reads that device's
`MTLDevice.currentAllocatedSize`, not a new device in the UI process. It measures
device resource allocation, not unique resident physical bytes or system-wide
GPU use. No-copy aliases, overlapping wrappers, heap allocation, driver caches
and resource lifetimes prevent interpreting it as a unique physical total.
The provider tests compare the public API with the same device getter, including
shared/private resources, overlapping no-copy buffers, heaps, allocation failure,
concurrent allocation/release and teardown. CPU and other unimplemented providers
report unsupported; this is not CUDA/Vulkan telemetry support.

## Collection and identity

One daemon sampler reads the bounded getter every two seconds and at request
boundaries. A separate sequence in the private inherited lifecycle mapping keeps
memory publication independent of phase publication. Atomic reads are bounded;
no inference mutex, GPU synchronization, disk write or UI timer is involved in
collection. The sampler is joined before freeing the backend. This also supplies
samples while model load or prefill is synchronous. Simultaneous boundary/timer
calls use a non-waiting publisher guard. The supervisor samples process RSS on its
independent monitor and validates PID plus process generation before accepting
either scope. Reopening the UI never starts another sampler.

The `memory` object in `/app/status` and schema-2 observations contains:

| Field | Meaning |
| --- | --- |
| `process_generation` | Opaque app-instance and owned-process generation |
| `sampled_at` | Observation wall time; ages use monotonic clocks |
| `process_rss_bytes` | macOS `proc_pid_rusage.ri_resident_size` or Linux `/proc/pid/stat` RSS |
| `process_rss_sampled_peak_bytes` | Maximum observed request RSS, not exact peak |
| `rss_source`, `rss_samples`, `process_rss_sample_age_ms` | Source, request sample count and age |
| `gpu_allocated_bytes` | Fresh supported device allocation, otherwise null |
| `gpu_allocated_sampled_peak_bytes` | Maximum supported request sample, not exact peak |
| `gpu_source` | `metal.MTLDevice.currentAllocatedSize` where supported |
| `gpu_samples`, `gpu_sample_age_ms` | Distinct observed snapshots and current age |
| `sample_interval_ms` | 2000; additional boundary samples are possible |
| `unified_memory` | Provider fact where supported, otherwise null |
| `process_physical_footprint_bytes`, `total_unique_physical_bytes` | Always null; not measured |

`status` is 0 missing, 1 measured, 2 unsupported, 3 query failed, 4 stale.
`source` is 0 unknown or 1 Metal. Human-readable `gpu_unavailable_reason`
distinguishes missing, unsupported, query failure and stale. `rss_unavailable_reason`
supplies the same missing/query-failed/stale distinction for process RSS. A measured zero is
numeric zero, never the unavailable marker. Samples older than 6000 ms are stale;
the UI also expires its own cached values if status polling stops. A historical
sampled peak can remain known when the final query fails; it is not the current
reading. Live status has no request-wide RSS peak. Each recorded observation
retains its actual backend and geistlib version/revision from engine provenance.

The existing top-level `rss`/`peak_rss` journal fields retain their meaning.
Legacy records with no memory object keep unknown GPU allocation and legacy RSS
source; they are not backfilled with guesses. Numeric-only observations still use
the bounded asynchronous journal, retention, rotation and no-follow persistence.
Exports never contain prompts or replies. The legacy `gpu_memory` field remains
null and directs clients to the scoped object.

## Suitability and diagnostics

Available memory is not increased by the active process RSS when assessing a new
model. RSS is not guaranteed reclaimable headroom, especially with shared memory.
Catalog estimates and current OS availability are conservative admission inputs;
unknown counters and tiny Metal-mode RSS do not certify that a large model fits.
An already-loaded model can remain usable without charging its allocation twice.

For matched developer overhead trials only, `GEIST_RESOURCE_SAMPLING=0` disables
the daemon resource sampler; it does not fabricate zero consumption. The default
is enabled. `tests/app/memory_real_test.py` records alternating enabled/disabled
trials using cached local models and isolated owned processes. Report observed
variation instead of claiming negligible overhead from code inspection. Exact
restoration of system-free RAM is not a leak test; process exit and generation
ownership are checked separately.
