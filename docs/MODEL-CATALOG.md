# Model catalog and execution

`models/catalog.json` is the single bundled source. `scripts/embed-models.py`
embeds it in the application so offline startup does not depend on a writable
external file. The catalog belongs to the app service, not geistlib.

## Import a newer catalog

Open **Settings → Model catalog** and select a UTF-8 JSON file. The Mac app opens
a native single-file chooser. Importing changes the available model list; it
does not download a model, start inference or execute any file. Downloads still
require an explicit model click. No online catalog updater is configured.

Use the bundled file as an example. Keep `schema: 1`, increment the positive
integer `revision`, and retain the `models` array. Each entry needs:

| Field | Meaning |
| --- | --- |
| `id`, `name` | Stable model ID and displayed name |
| `file` | Unique basename ending in `.gguf` |
| `url` | HTTPS Hugging Face `/resolve/` download URL |
| `sha256`, `bytes` | Exact lowercase SHA-256 and file length |
| `working_mib` | Estimated working memory, including context |
| `recommended_ram_gib` | Total system RAM guidance |
| `backends` | Supported implementations: `cpu`, optionally `metal` or `vulkan` |

Version 1 accepts at most 32 entries and 24 KiB, plain UTF-8 strings, safe
ASCII IDs/basenames, unique keys/IDs/files and positive bounded integers.
Control characters, escaped strings, arbitrary download hosts, path traversal,
unknown keys and unknown backends are rejected. Every entry must include CPU.
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

## CPU and GPU

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

Successful short-test replies retain one numeric observation per model SHA and
actual processor backend. The service writes private atomic `performance-*`
files: rate, first-text latency, total time, output tokens, a post-reply process
RAM snapshot and timestamp. Prompts and responses are never included. Files are
scoped to the application version and hardware/OS identity; different versions,
corrupt/nonfinite values and mismatched identities are ignored. Failed/cancelled
replies do not replace a previous observation. Model switching and application
restart retain valid observations; importing changed model hashes cannot reuse
old measurements. `performance_history` in authenticated status describes only
the selected model. External editor replies are not recorded in this comparison.
