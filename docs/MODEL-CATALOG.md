# Model catalog and execution

`models/catalog.json` is the single bundled source. `scripts/embed-models.py`
embeds it in the application so offline startup does not depend on a writable
external file. The catalog belongs to the app service, not geistlib.

## Import a newer catalog

Open **Settings → Model catalog** and select a UTF-8 JSON file. The Mac app opens
a native single-file chooser. Importing changes the available model list; it
does not download a model, start inference or execute any file. Downloads still
require an explicit model click. No online catalog updater is configured.

Use the bundled file as an example. Use `schema: 2`, increment the positive
integer `revision`, and retain the `models` array. Each entry needs:

| Field | Meaning |
| --- | --- |
| `id`, `name` | Stable artifact ID and legacy full display name |
| `group_id`, `group_name` | Explicit model identity and heading shared by its variants |
| `quantization` | Unique format label within that group, such as `Q4_0` or `Q8_0` |
| `file` | Unique basename ending in `.gguf` |
| `url` | HTTPS Hugging Face `/resolve/` download URL |
| `sha256`, `bytes` | Exact lowercase SHA-256 and file length |
| `working_mib` | Estimated working memory, including context |
| `recommended_ram_gib` | Total system RAM guidance |
| `backends` | Supported implementations: `cpu`, optionally `metal` or `vulkan` |
| `reasoning_format` | Optional `none` (default) or `think_tags`; a verified leading output protocol |
| `unsupported_format` | Optional `pq2_0` marker for an unavailable format; requires empty `backends` |

Both versions accept at most 32 entries and 24 KiB, plain UTF-8 strings, safe
ASCII IDs/basenames, unique keys/IDs/files and positive bounded integers.
Control characters, escaped strings, arbitrary download hosts, path traversal,
unknown keys and unknown backends are rejected. Runnable entries must include CPU.
An entry with `unsupported_format: "pq2_0"` and `backends: []` remains visible,
but cannot be downloaded or started. This marker is retained for catalogs that
explicitly disable PQ2_0/Hadamard execution; the bundled revision 4 no longer
marks Bonsai this way. Catalog metadata does not install another engine.
Older apps without this schema-1 extension reject such
an import; update the app first.
Import only a catalog whose source and model hashes you trust; a hash from the
same untrusted file is not a publisher signature. Catalog metadata alone does
not add engine architecture, audio or image support.

The authenticated service validates the complete file, then writes a private
`catalog.json` atomically in the application data directory. Older/equal
revisions, edits to an active model and a changed hash reusing a filename with
existing full/partial data are refused. Busy inference/download/loading blocks
import. A rejected import leaves the prior catalog intact. On restart, an
invalid or older saved catalog falls back to the bundled one. Existing model
files are never deleted by import. The authenticated `GET /app/catalog` exports
the effective catalog; `POST /app/catalog` imports its raw JSON.

## Visible variants and schema migration

Schema 2 retains the `models` artifact array to keep API IDs, files, hashes,
receipts and measurements stable. Group by `group_id`, never by parsing names.
All members must share one `group_name` and have different `quantization` values.
These three fields are required; group IDs and quantization labels use the same
safe ASCII component rules as IDs (63 and 32 bytes respectively). Group names
are plain UTF-8, up to 100 bytes. Group metadata is display-only; it does not
change the artifact passed to `/app/select`, `/app/remove` or `/v1` clients.

For example, these metadata fields belong on otherwise complete artifact entries:

```json
{"id":"qwen38-27b-q4","group_id":"qwen38-27b","group_name":"Qwen3.8 27B","quantization":"Q4_0"}
{"id":"qwen38-27b-q8","group_id":"qwen38-27b","group_name":"Qwen3.8 27B","quantization":"Q8_0"}
```

Only quantizations of the same checkpoint belong together. Bonsai 2 is a
separately trained derivative and has its own group. CPU/GPU is an execution
choice for an artifact, never a quantization. Q4_K_M is a mixed quantization;
its exact format is shown rather than claiming uniform four-bit weights.

Schema 1 remains readable/importable with a newer revision. Each legacy entry
becomes its own group, with an unspecified/default variant. Group metadata in
schema 1 is rejected rather than silently ignored. To group a custom catalog,
upgrade it explicitly to schema 2 and increment its revision. Bundled revision
4 supersedes older saved catalogs without rewriting them or deleting downloads.
A newer custom schema-1 catalog keeps precedence and its separate entries.
Older apps reject schema 2: update the application before importing it.

The UI shows every variant row without a dropdown. Each row shows its format,
file size and independent download state. A green complete ring means downloaded;
only a ready runtime marks the row Active. A loading ring does not claim activation.
Resource warnings include a short visible reason. Deleting one row removes only
that artifact's downloaded/partial data, retaining the row and its sibling variants.
Measurements remain artifact/backend-specific, not group averages.

## CPU and GPU

### Reusing verified model files

Each newly downloaded or previously unverified catalog file receives a full
SHA-256 check. The service then saves a private, atomic verification receipt
bound to the catalog digest, file device/inode, size, owner, permissions, link
count and nanosecond modification/change timestamps. Switching back to an
unchanged model, including after a service restart, checks that receipt instead
of rereading the full model. Replacing or changing a file, an invalid receipt,
or a new catalog digest requires full verification again. Same-size edits with
restored mtime invalidate through ctime. Symlinks are never accepted.

This receipt is a local cache, not proof against a process with the same user
privileges. Failure to save it only costs another full check. Model loading into
CPU/GPU memory still takes time. The UI distinguishes loading from checking a
model and downloading new bytes; existing files are never labeled as a download
merely because they need verification.

The active-model header offers **Auto · CPU · GPU**. Auto currently suggests
Metal for supported catalog models of at least 1 GiB; smaller models and the
ternary BitNet package use CPU. This is an explicit hardware heuristic, not a
benchmark-derived fastest-processor claim. Generation speed refers to the last
reply; changing backend invalidates that display. Drafts and session messages
remain in the open window.

The app probes the packaged daemon once using `geistd --backends`. A GPU option
requires both runtime availability and catalog support. Apple Silicon packages
include Metal. Current Linux packages remain CPU-only; a Vulkan engine build and
hardware validation are prerequisites before exposing a working Linux GPU
option. Adding `vulkan` to JSON alone does not enable it. Custom GGUF files without
catalog capability metadata use CPU.

The active processor has a check within its option, while the selected radio
records the policy (including Auto). A star marks the hardware recommendation;
it is not a measured-speed award. Reloading replaces the active check with a
spinner until the backend is ready. GPU backend names, such as Metal, appear as
option sublabels. Screen-reader labels and a polite status announce the same
information without duplicating visible text beside the control.

Switching an idle model restarts the same owned `geistd` with an explicit
`GEIST_BACKEND`. It neither downloads nor hashes the model again. The daemon
reports its actual backend through `info`; a mismatched reported backend is
refused. Preference persistence is bound to the model SHA, and happens after a
successful load. Old daemons without backend reporting can still run on CPU.
Busy chat/editor requests block switching. GPU load failure restores CPU once,
shows an explanation and preserves a separate `gpu-failure-*`. Model load
has a 120-second deadline. The API is `POST /app/execution` with
`{"mode":"auto"}`, `{"mode":"cpu"}` or `{"mode":"gpu"}`.

CPU quality evidence is not used to certify GPU answers. No new human quality
rating or 24-hour acceptance result is implied by these functional tests.

## Remembered processor measurements

App and editor replies populate the bounded private numeric history described in
[Local performance profiles](PERFORMANCE-PROFILES.md). The comparison uses up to
30 compatible observations per artifact, engine configuration, backend and workload
group. App-only updates and Mac re-signing of unchanged engine payload preserve
compatibility; engine/configuration changes remain separately attributed.

`performance_profile` provides medians, ranges, counts and recent diagnostics.
`performance_history` remains a compatibility view of the last eligible reply per
processor, used by the existing execution-speed warning. Prompts/replies are never
persisted. Failed/empty/cancelled observations remain diagnostic and cannot replace
successful comparison values. Existing `performance-*` files import as archived
legacy measurements because they lack complete engine/configuration identity.


## Device suitability versus execution speed

`resource_fit` describes platform/format support, memory, disk and, only when
known, the best available processor's speed. Unknown speed is not a resource
restriction. A slow CPU response does not downgrade a model if its available GPU
is fast or unmeasured. A general speed caution requires every available supported
processor to have a valid last reply below 8 tokens/s. Memory/format constraints
take precedence. The loaded model's resident memory is credited when checking
its own remaining memory budget; it is not charged twice.

The loaded-model header uses `execution.performance`: `target_tps`,
`below_target` and `rate` (null when unknown). The actual ready, verified backend's
last reply controls this warning. Loading or switching hides it until the actual
backend is known. CPU/GPU observations remain visible together. These observations
are workload-dependent, not controlled benchmarks; the 8 tokens/s threshold is
an application heuristic, not model quality or a hardware limit.

## Revision 3: Bonsai execution and the three 27B entries

See [catalog sources](../models/SOURCES.md) for pinned artifacts and limitations.
Q4_0 and Q8_0 are distinct standard quantizations of Qwen3.8 27B. Bonsai 2 is a
separately trained ternary derivative, not another ordinary Qwen quantization.
The pinned engine `e26436906ff6fe7eda296b90fa3a7a9dfa69f418` supports the Qwen
hybrid architecture, Q4_0/Q8_0 and Bonsai's PQ2_0/Hadamard execution. Bonsai is
enabled for CPU and Metal; its working-memory planning allowance is 20 GiB and
total-RAM guidance 24 GiB. This corrects the earlier 12 GiB estimate, which a
short CPU inference already exceeded. These values are not measured peaks;
process RSS cannot establish total Metal memory. None of these entries replaces
the platform default or claims verified response quality.

Older saved catalogs fall back to the bundled revision without deleting model
files. Equal/newer custom catalogs keep their existing precedence. Ship the
updated runtime and catalog together; importing an enabled entry cannot add
PQ2_0 support to an older engine.

## Revision 5: answer preparation

Qwen and Bonsai entries declare `reasoning_format: "think_tags"`. The app service
separates leading `<think>…</think>` blocks before app and editor serializers.
This is based on inspected GGUF templates and the current ChatML assistant prefix;
no block is assumed to be pre-opened. Repeated leading blocks and whitespace are
supported; literal tags after an answer begins or inside code remain text.
Unknown profiles are rejected. Missing profiles in older catalogs mean `none`.
Custom files without trusted catalog metadata are unchanged. Gemini/Gemma-style
channel delimiters are not inferred from names. Update the app before importing a
catalog containing this field; old apps reject unknown keys safely.

See [answer handling](ANSWER-HANDLING.md) for response budgets, errors and metrics.
