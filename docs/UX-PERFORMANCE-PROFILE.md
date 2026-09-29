# Local performance profile — concept

Status: implemented in local candidate 0.5.23, 29 September 2026.
See [implementation and verification](PERFORMANCE-PROFILES.md) for final defaults,
bounds and limitations. The source inspection below describes the preceding version.
The accompanying interactive concept uses explicitly synthetic example values.

## Purpose and primary flow

Help a person answer three questions: how responsive is this model on my computer,
which processor should I try, and how much memory is it using? Keep the short test
and model manager primary. The profile grows from normal use without background
benchmark runs, downloads or processor switches.

Flow: select an installed model → see CPU/GPU observations beneath its name → use
chat or an editor → one completed request adds an observation → inspect the local
profile only when needed. Per-answer timing stays attached to that answer.

## Existing implementation, inspected

- `src/app/main.c` stores one successful measurement per artifact hash and backend
  in a private `performance-<sha256>-<backend>` preference file. The next successful
  result replaces it. Values include tokens/s, first text, total time, output tokens,
  process RSS after the answer and a timestamp. These are not a historical series.
- Restore currently requires the same app version and hardware/OS identity string.
  An app update does not delete these files, but their previous values are excluded
  from the current profile and can be replaced by a new measurement.
- `/app/generate` calls `remember_measurement`; `/v1/chat/completions` does not.
  Consequently editor traffic does not yet progressively populate this profile.
- The UI mixes a live process/system snapshot, last CPU/GPU replies and an additional
  nested Measurements section. CPU/GPU figures describe different requests; they
  do not prove that one processor is faster for an identical workload.
- OS sampling exposes process resident memory and CPU. GPU memory, total device
  utilization, energy and an actual whole-run memory peak are not measured.

## Presentation

Use a white surface, dark neutral text, tabular figures, subtle separators and
restrained blue for interaction. Green means the model is ready. Keep concise
labels beside icons: seconds, RAM and typical speed need meaning beyond a symbol.

### Always visible below the selected model

1. Existing Auto / CPU / GPU controls. Put the backend (Metal, Vulkan, CUDA where
   actually supported) and that processor's typical tokens/s inside its option.
   Distinguish requested policy from active processor when Auto is selected.
2. One quiet metric row: first text for the active processor, **RAM now**, artifact
   size, and a labelled disclosure for the profile. Typical latency and live RAM
   must have distinct labels; tooltips supplement rather than replace them.
3. The existing `45.0 tok/s · 897 tokens · 21.46 s` style remains under each answer
   and always represents that specific response, regardless of later aggregates.

Before enough samples exist, processor options say `—` or `First observations`;
never show estimated speed as a measured value or use zero for missing data.

### Profile on demand

Replace the nested Measurements section with one compact comparison table. Rows:

| Row | Meaning |
| --- | --- |
| Typical speed | Median within the selected compatible workload group |
| Usual range | 25th–75th percentiles of those observed speeds; not a confidence interval |
| First text | Median service-observed time until the first visible text |
| Total time | Time to complete the same sample group; output size stays visible |
| Output tokens | Typical count and the selected output-length group |
| RAM after response | Process RSS snapshot, not GPU memory or a guaranteed requirement |
| Observations / updated | Count and timestamp for each processor separately |

A compact line below gives the current computer, OS, total/available RAM and live
model CPU use. Long sampling explanations move behind one accessible info action.
Optional history is a directly labelled sequence of observations with time and
workload, not a smoothed trend implying that the model itself is learning.

On desktop, open a bounded non-modal popover anchored below the metric row;
it must not move the composer or steal focus during polling. On a narrow screen,
a user-opened sheet may use the available width. If modal, apply real modal focus
and inert-background behavior, Escape/close and focus return. Closing the sheet
never stops inference. Use the same content and controls at every width.

Do not add a separate Performance navigation tab or a global System section.
Settings holds only collection on/off, retention, export and delete-history controls.

## How the profile grows

### Collect one numeric record per request

Use one collector for both the short test and OpenAI-compatible editor endpoint.
Each completed request receives a unique measurement ID. Capture its model/hash,
actual backend and settings at request start; never infer them from the currently
selected UI model after completion. Preserve per-response timing even if the UI
closes. All timing calculations use monotonic clocks; wall time is metadata only.

Record numbers, configuration and status, without prompts, replies, file names,
repository paths, document contents, user names or API keys. Client source is only
`app`, `api`, or `controlled_test`; never guess the editor from prompt contents.
No network upload. Users can pause collection, export or delete local history.

Live resource sampling runs in the service on a bounded cadence (initially 2 s),
independent of status polling or whether the window is open. Store one summary per
request, not every idle poll. New peak statistics must be labelled **sampled peak**
with their sampling interval; RSS and GPU memory remain separate metrics and must
not be added on unified-memory systems. Unsupported metrics stay null.

### Keep comparisons meaningful

Partition records by artifact SHA/quantization, actual backend/device, engine build,
context/KV configuration, offload and thread settings, and relevant OS/driver/runtime
versions. The interface app version is metadata, not an automatic reset key.
Engine or configuration changes open a new series; old series remain readable.
No automatic merge across incompatible series.

Within a compatible series, retain input-token count, output-token count, cache
reuse, cold/warm state and observed contention (for example an overlapping model
download). Compare approximate workload groups, initially input counts <=512,
513–2048 and >2048, crossed with output counts <32, 32–127, 128–511 and >=512.
These boundaries are proposed product defaults and need usability/performance
validation. Short replies and cold loads are preserved with their own labels.

A profile shows the most recently used compatible workload group, explicitly
labelled. It must not silently switch to a different group to fill a missing CPU
or GPU cell. Different ordinary requests remain observational, even within a group.
A background-download flag should be captured over the entire request interval,
not only at its start or end. Contended observations remain inspectable separately.

Start with a median over the last 30 eligible observations in that series/group.
With 1–4 observations, show `First observations` and the actual count. From 5,
show `Typical` plus count/range. Five is a display threshold, not statistical proof
or response-quality certification. No inferred improvement/speedup badge from
uncontrolled chat traffic. The median limits a single outlier's effect; raw values
remain accessible. Recompute aggregates rather than editing earlier observations.

Failed, cancelled, disconnected and interrupted requests have explicit outcomes
and partial accounting where available; they do not count as successful typical
response measurements. An ordinary token-limit completion remains a completed
sample with `finish_reason=length`. Empty outputs remain diagnostic only.

### Controlled CPU/GPU comparison

Offer a secondary `Measure comparison` action only when idle. Explain that it
loads each eligible processor, runs a versioned fixed prompt/output configuration,
then restores the previous processor policy. Keep warmup, model load and warm
inference times separate. Start with one warmup and three measured repeats per
backend, using a fixed cache policy. Label it a short local benchmark; retain run
configuration and dispersion. Do not launch this automatically or compete with
chat/downloads. A user cancellation preserves the previous working model.

Normal observations guide expectations. A recommendation based on comparative
speed requires matched controlled runs and sufficient memory headroom; it must
state its evidence. Never silently switch an explicit CPU/GPU choice. First
version keeps the existing hardware recommendation and distinguishes it from
measured speed. A later decision policy can use controlled evidence with a clear
margin/hysteresis to avoid switching on measurement noise.

## Local storage and ownership

Own collection/persistence in the C23 application service in **geist-serve**, beside
model management. Every client contributes through that common path. The frontend
only reads/filters numeric summaries. geistlib should expose engine counters where
needed; it should not own profile files, retention, UX labels or recommendations.
Do not make private engine calls or introduce this product policy into geistlib.

Proposed storage beneath the existing private Geist data directory:

```
performance/
  observations.jsonl       # Numeric request records, append-only between rotations
  observations.1.jsonl     # Previous bounded segment
  profiles.json           # Derived cache; rebuildable from valid records
```

On macOS: `~/Library/Application Support/Geist/performance/`.
On Linux: `${XDG_DATA_HOME:-~/.local/share}/geist/performance/` (descriptive notation).
The override follows the existing application home. The model catalog JSON remains
separate: catalog data is distribution input; performance history is local output.

JSONL is the initial recommendation: inspectable, exportable, small dependency
footprint, and one record per response. Start with 90-day retention and at most
20 MiB across both journal segments; display the current retained period/count.
The derived cache is separately capped (2 MiB). These are proposed defaults,
not an unlimited archival guarantee. Compact on rotation/idle; never rewrite the
entire history at every 2-second resource refresh. If richer queries eventually
justify SQLite, migrate the storage implementation while preserving JSON export.

A single service-owned writer handles a bounded queue (initially 64 records, each
<=8 KiB), with typed ownership and checked sizes. Enqueue after response accounting;
no file flush or aggregate scan may hold the inference mutex. Apply restrictive
permissions (directory 0700, files 0600), reject symlinks, and coordinate with the
existing single-service lock. Use atomic replacement for derived caches/rotation
manifests. Flush on clean shutdown and bounded intervals. After a hard crash, a
small unflushed tail may be lost; completed valid records remain readable. Ignore
and preserve diagnostics for a truncated final line; never interpret it as a zero.
Unknown/corrupt cache can be rebuilt without changing journal evidence.

If disk is full or the queue is exhausted, chat continues. Show one persistent,
non-repeating `History could not be saved` state, retain bounded in-memory values
and expose a dropped-record count. Never claim those records were persisted.
Export is user-initiated. Deleting history confirms its scope and removes history
only; models, current chat and catalogue remain intact.

### Record contract, proposed schema 1

| Field group | Required content |
| --- | --- |
| Identity | schema, measurement ID, timestamp, local profile ID |
| Model | artifact SHA, catalog ID, quantization |
| Runtime | engine build, actual backend/device, context/KV/offload/thread settings |
| Environment | relevant hardware/OS/runtime versions; no device serial/hostname |
| Workload | input/output token counts, reused tokens, cold/warm/cache policy |
| Durations | generation ns, first visible text ns, total ns; optional prefill/load ns |
| Resources | RSS after response; sampled peak/CPU summary only when actually collected |
| Conditions | background download overlap, other known contention, availability of sensors |
| Result | source, completed/cancelled/error/disconnected, finish reason |

Unavailable counters use null plus a reason. Units are fixed in storage; DE/EN
formatting happens only in the UI. Do not sum or convert counters with incompatible
scopes. Preserve the service vs client-observed distinction for first-text and
roundtrip timing.

### Migration

Import each valid old preference as one `legacy_last_reply` record, once, only
when its recorded identity can be attributed. Keep original files until migration
is verified. Missing engine/configuration identity means an archived legacy value,
not membership in a new compatible series. Use an idempotent migration marker;
restart must never duplicate imported samples. An app-only update preserves current
series; engine/backend changes retain older series as historical evidence.

## States, accessibility and acceptance

1. Fresh installation: all unmeasured values show a dash and `Not measured yet`;
   chat/download work normally. No hidden calibration starts.
2. A completed app/API request adds exactly one numeric record to its captured
   model/backend series. Reconnect/retry cannot duplicate a completion ID.
3. CPU/GPU and quantization switches preserve distinct histories; background
   downloads do not clear profiles or interrupt inference and are recorded as
   contention only when they overlap the request.
4. Restart restores history. UI-only upgrades retain it; engine/configuration
   changes create an attributed series and preserve older evidence.
5. Golden-data tests verify group boundaries, medians/percentiles, sample counts,
   short/failed/limited replies, timestamp handling and missing values. A very
   fast outlier cannot become an unqualified fastest-device recommendation.
6. Storage tests inject full disk, permission failures, interrupted writes,
   malformed records, symlinks, rotation interruption and concurrent API traffic.
   Writes remain bounded; an invalid journal/cache never prevents model startup.
7. Inspect every written/exported field: no prompt/reply contents or secrets.
   Opt-out suppresses durable records; delete-history preserves model data.
8. Rendering and aggregate updates keep transcript, composer, selection and focus
   stable. Model identity stays visible. Measurements use aligned tabular figures.
9. Keyboard/VoiceOver: labelled controls, meaningful table headings, Escape/close
   and focus return; no color-only status or hover-only essential information.
   Announce completed updates politely, not every live resource sample.
10. Verify DE/EN, 320/390 px, 540×480 desktop, 200% text/zoom, long values and reduced
    motion. Small screens preserve all values with reflow, not clipping. Test modal
    behavior only when the sheet is actually modal.
11. Benchmark tests check explicit start, idle gating, fixed workload, warm/cold
    separation, abort/failure recovery and previous policy restoration. Normal-use
    observations must not be labelled controlled comparisons.
12. Measure collector overhead with identical workloads on/off, including slow
    storage. Acceptance: no synchronous per-token file writes, unbounded growth or
    foreground pauses; define a numerical overhead budget from baseline trials
    before claiming negligible impact.

## Suggested implementation sequence

1. Consolidate the existing presentation, preserve answer metrics and establish
   one common app/API measurement event with unit/clock definitions.
2. Add bounded journal/cache, migration and numerical aggregation tests. Keep the
   currently displayed last reply distinct while enough profile data accumulates.
3. Add typical values, count/age, workload group, history and storage controls;
   verify keyboard, languages, narrow layout and fault states in the real hosts.
4. Add explicit controlled comparisons after the persistence path is reliable.
   Automatic performance-based advice is a separate later policy decision.

## Design basis

This is a product proposal informed by source inspection and the following design
references, not a claim of measured usability improvement:

- [NN/G: Progressive Disclosure](https://www.nngroup.com/articles/progressive-disclosure/)
  supports keeping common information visible and deferring occasional detail.
- [W3C: Accessible Tables](https://www.w3.org/WAI/tutorials/tables/)
  supports explicit row/column relationships for CPU/GPU comparison.
- [W3C: Modal Dialog Pattern](https://www.w3.org/WAI/ARIA/apg/patterns/dialog-modal/)
  defines focus containment, close/Escape and focus return for a genuinely modal
  narrow-screen sheet. The desktop popover must not pretend to be modal.
