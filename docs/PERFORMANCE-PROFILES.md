# Local performance profiles

Implemented by the C23 application service in local candidate 0.5.23. The engine
and catalogue remain independent. This implements the approved
[UX concept](UX-PERFORMANCE-PROFILE.md); it does not certify response quality.

## User experience

CPU/GPU options display observed median generation speed. The quiet row beneath
shows service-observed first text, current process RAM and artifact size. **Profile**
opens a bounded non-modal comparison table, preserving the composer and keyboard
focus. Escape or clicking outside dismisses it. The table includes quartiles,
latency, output size, after-response RSS, sampled peak, count and date. One sample
has no displayed range. Counts below five say **First observations**. DE/EN and
narrow layouts use the same controls and semantic table.

Every answer keeps its own existing tokens/s, tokens and total-time footer.
**Recent observations** lists the last twelve records for this artifact, including
failed/archived records; export contains the entire retained numeric inventory.
An earlier engine/configuration is explicitly historical. There is no separate
Performance tab and no automatic benchmark or processor switch.

**Settings → Local measurements** controls collection, 30/90/365-day retention,
export and confirmed deletion. Default: collection enabled, 90 days. Opt-out also
excludes a request that was still running when the setting changed. Chat contents
never enter the collector. Deleting history leaves models, catalogue and chat
untouched. The service-created export is a private `performance/export.jsonl`;
the UI shows its complete local path. Another export replaces this snapshot.
Deletion removes that snapshot as well; copies made elsewhere are user-owned.

## Measurement contract

Both `/app/generate` and `/v1/chat/completions` capture the artifact/backend/config
at request start and submit exactly one typed record at completion or interruption.
Timing uses monotonic clocks; timestamps are metadata. Streaming and nonstreaming
editor requests are covered. First text is measured at the service, not at an
external editor's screen. An unidentifiable custom file has no comparable profile.

Engine executable payload SHA-256, hardware description, OS, RAM, CPU count, context size,
thread limit, session count and engine-default KV/offload policy form the series
identity. Artifact SHA and actual backend partition it further. The app UI version
is deliberately absent from this identity. On packaged 64-bit Mach-O, the code-signature
blob and its link-edit size fields are excluded from this compatibility hash:
re-signing unchanged engine code must preserve its history. Ordinary files/ELF use
the full file SHA-256. This is not a replacement for signature or model-file
integrity verification. Engine-default configuration changes
change the executable hash. Driver/device identifiers not exposed by the engine
are not invented: current packaged Linux execution is CPU-only; Apple Silicon
Metal uses the local SoC. Transferring the data directory between indistinguishable
hardware configurations is not a controlled device comparison.

Within a series, input buckets are ≤512 / 513–2048 / >2048 and output buckets are
<32 / 32–127 / 128–511 / ≥512. Cold-first-reply, actual prefix-cache reuse, known
overlapping model download and controlled/ordinary source are separate groups.
An overlap that starts and finishes inside a request is still captured. Other
applications' contention is not measured. The newest eligible group is shown;
an absent CPU/GPU counterpart stays blank. No borrowing from another group.

The last 30 successful nonempty observations in that group supply the median and
linearly interpolated 25th/75th percentiles. These are descriptive statistics, not
confidence intervals. Ordinary prompts can differ within a bucket. Interrupted,
disconnected, cancelled, failed, empty and legacy observations do not count.
A completed token-limit response does count, with finish reason `length`.

Service-side resource sampling runs independently of browser polling, about every
2 seconds plus request boundaries. Records contain an after-response RSS snapshot,
observed peak RSS, sample count and available CPU samples. A sampled peak can miss
brief spikes. RSS does not represent total GPU memory or a guaranteed requirement.
The owning Metal backend supplies separately scoped allocated resource bytes,
request-end values and sampled peaks through the independent cached status path.
Both scopes retain source, generation and age; they are never added together.
Unsupported, failed and stale values are null; known zero remains zero. No energy
or unique-physical-memory estimate is presented. See [memory telemetry](MEMORY-TELEMETRY.md).

## Explicit controlled comparison

**Measure comparison** requires confirmation and an idle installed model. It loads
CPU, then eligible GPU, using a fresh daemon for each. Each processor performs one
warmup and three measured responses with a versioned fixed prompt, temperature 0,
top-p 1 and maximum 128 output tokens. Each response uses a fresh session; actual
prefix reuse remains recorded. Warmup and model-load timing are separate from the
measured group. All records share a run ID, and different runs are not merged.

Chat, model switching, downloads and catalogue replacement are gated during this
explicit test. Cancellation interrupts inference and restores the previous
processor policy. A failed restoration is reported, never called success. The
existing hardware recommendation remains independent of these observations.
This is a short local test, not a general model/backend ranking.

## Storage and failure handling

Under the normal private application home:

```
performance/
  observations.jsonl
  observations.1.jsonl
  profiles.json
  settings.json
  export.jsonl             # only after explicit export
```

On Mac the home is `~/Library/Application Support/Geist`; on Linux it follows the
existing XDG data directory. `--home` also relocates this entire store.

- Directory 0700; files 0600. Symlinks, foreign ownership and hard-linked journal
  files are rejected. The existing single-app lock coordinates ownership.
- One writer owns disk operations; the inference mutex never flushes history or
  builds aggregates. Queue: 64 typed records; each serialized record ≤8 KiB.
- Two journal segments ≤10 MiB each. In-memory/query inventory: newest 8192 retained
  observations. Export ≤20 MiB; derived cache ≤64 KiB (stricter than proposed 2 MiB).
  These are bounded working histories, not an unlimited archive.
- The journal is authoritative. Startup rebuilds memory and ignores stale/corrupt
  derived cache. A truncated final line is skipped and sealed before future writes.
  Invalid-line counts are visible. Completed valid records are retained.
- Rotation atomically renames the current segment. Retention compaction uses staged
  files and atomic replacements, runs at startup, on retention changes and daily
  while the service runs. Stale owned temporary replacements are removed on restart;
  committed journals remain authoritative. A crash can lose an unflushed tail.
- Disk errors/queue overflow leave bounded in-memory values and keep inference
  available. The UI shows a persistent error and dropped-record count. Cache errors
  do not falsely count a successfully journaled record as dropped.
- Settings/export/delete serialize with the writer, never with the inference
  mutex. Export is numeric only. Its explicit snapshot is separate from automatic
  retention and is replaced on next export or removed by Delete history.
- Valid old `performance-<hash>-<backend>` preferences import once as
  `legacy_last_reply`. Their original identity is preserved, unknown engine/config
  remains archived, and source files remain untouched. Deterministic IDs avoid
  duplication; a deletion tombstone prevents legacy values from returning.

## API and verification

Authenticated local routes:

| Route | Purpose |
| --- | --- |
| `GET /app/status` | `performance_profile`, resource counters, comparison progress |
| `GET /app/performance/export` | Retained observations as JSONL |
| `POST /app/performance/export` with `{}` | Private file export, returns local path |
| `POST /app/performance/settings` | Boolean `enabled`, days 30/90/365 |
| `POST /app/performance/clear` | Requires `confirm: true` |
| `POST /app/performance/compare` | Requires `confirm: true`, idle installed model |
| `POST /app/performance/cancel` | Cancels only the comparison |

`make test-app` includes numerical, schema, persistence, privacy, opt-out,
permission/full-disk/symlink, torn-write, rotation, slow-writer/queue, engine-change,
app-version, migration, comparison and cancellation tests. The existing background
transfer regression additionally checks whole-request overlap attribution.

`tests/app/performance_real_test.py` opts into the cached reference GGUF through
`GEIST_TEST_MODEL`; it verifies app/API CPU and eligible GPU, actual warmup/repeats,
restoration, export and restart. Deterministic peers are protocol evidence only.
Native Mac and shared Linux UI tests check the compact surface and answer footers.

The slow-writer test enqueues 200 numeric records against a deliberately delayed
writer and checks a generous <1-second ceiling. This detects synchronous writer
regressions; it is not a claim that end-to-end inference overhead is negligible.
Matched collector-on/off inference trials remain required for a numerical overhead
claim. Linux hardware and remote CI results must be reported separately from local
Mac evidence.

## Answer preparation metadata (0.5.25)

`first_ns` retains time to the first model text, including hidden preparation.
Optional `first_answer_ns` records the first visible answer separately; missing
legacy values stay unknown. `reasoning` indicates a recognized hidden block.
Generated tokens and generation time include preparation. `no_answer` observations
are retained diagnostically and do not contribute to successful-answer aggregates.
No reasoning text is saved in this schema or exports.

## Actual engine provenance (observation schema 2)

`geistd --build-info` reports the linked library's version, the resolved source
revision/state and the SHA-256 of its static archive. `info.engine` reports the
same identity after loading. The build partitions object directories by source
content and verifies unchanged inputs before linking; header/link version mismatch
fails closed. Release packages include `ENGINE.json` and require clean provenance.
Source archives without Git remain unknown, never a guessed clean revision.

The app freezes this identity and its signature-independent executable payload
hash at request admission. Schema-2 records and compatible aggregates contain
`engine.geistlib.{version,revision,source_state}` and `engine.payload_sha256`.
Unavailable identity fields are null with `unavailable_reason: not_reported`.
The bounded parser rejects malformed provenance. Older daemons still operate
with explicitly unknown identity. Different revisions never share an aggregate,
even if semantic versions match. The payload remains an independent safeguard.

Schema-1 rows are read and exported as schema 1, without retroactive metadata.
Mixed journals, rotation, retention and derived-cache reconstruction preserve
this distinction. Provenance contains neither checkout paths nor user content.
