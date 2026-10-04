# Mini benchmark (#35)

What can each local model do on this computer? Four small tasks in German and
English, scored automatically, with latency and memory measured on the same
run. It compares models; it does not certify a model for a particular use.

```sh
make && make app
python3 workbench/bench.py run --candidate gemma4-e2b --candidate qwen3-0.6b --backend gpu --output build/bench/2026-10-01
python3 workbench/bench.py report build/bench/2026-10-01        # --json for machines
python3 workbench/bench.py quality build/bench/2026-10-01 --write  # catalog evidence (#102)
```

`quality` turns complete runs of a committed engine into the catalog's
optional `quality` field: passed/total per task and language, the suite id,
the date, the engine pin and the sha256 of `results.jsonl`. The app shows it
as the reference test; a result for another suite id is an older test. Answer
quality hardly depends on the computer, so it is measured once, on a
reference machine, not on every user's computer.

Candidates are catalog ids (`models/catalog.json`) whose files are already
downloaded in geisten (`--model-dir`, default: the geisten home's `models/`).
Downloading stays an explicit action in the app.

## Tasks

`workbench/suite/*.json`, 20 cases per language each, frozen and versioned.
`workbench/build_suite.py` regenerates them byte for byte. These are
authored examples, not real user traffic, so a high score here is a
comparison, not a promise for other inputs.

| Task | Input → expected | Passes when |
|------|------------------|-------------|
| `classify` | support message → `{"category","priority"}` from fixed lists | both labels match (case-insensitive) |
| `extract` | reservation or invoice text → JSON with fixed keys; ISO dates, 24 h times, numbers, `null` when missing | exactly the expected keys, every value equal (strings case-insensitive, numbers numerically, `null` as `null`) |
| `format` | an instruction: N bullets, a JSON object with two keys, at most N words, one line in capitals | the rule holds exactly |
| `context` | a fictional text plus a question; 12 answerable, 8 not | answerable: the required words appear (word boundaries, listed alternatives); not answerable: the reply is exactly `NOT IN TEXT` / `NICHT IM TEXT` |

`classify` includes a prompt injection, a negation, spam and praise hiding a
problem. For `context`, the report counts unanswerable questions answered
**without the marker**. Read those replies: some are invented answers, others
are refusals in other words. A script cannot reliably tell the two apart.

The suite tests models, not the geisten UI. Each case is one request to
`/v1/chat/completions` with the task's system prompt, the case as the user
message, temperature 0, and the task's output limit. There is no retry and no
repair: JSON inside a code fence fails.

## Run

Each candidate runs alone in a private `geist-app` + `geistd` from this
checkout, with a temporary HOME on the same file system as the models (the
model file is hard-linked, not copied) and selected by catalog id. The app
verifies the file against the catalog sha256 before loading, exactly as for a
download, and knows which backends the model supports. Candidates run one
after another; at most one model is resident at a time. Close a running geisten
with a large model first if memory is tight: the benchmark does not yet
coordinate with the shared service.

One declared warm-up request precedes the 160 scored cases.

Output (the directory must not exist; nothing is ever overwritten):

```text
<output>/<candidate>/run.json        identity and conditions
<output>/<candidate>/results.jsonl   per case: raw output, finish reason, tokens, ms, pass, reason
```

`run.json` records the suite version and hashes; model catalog id, file,
sha256, bytes, quantization and source; source commit and dirty state, engine
pin, `geist-app`/`geistd` hashes; requested and actual backend; OS,
architecture, CPU model, logical CPUs and memory (no host name, user name or
home path); sampler; preparation time (hash check + load); memory; start/end;
errors.

## Report

One row per model: passed cases per task (de · en), total, median and
nearest-rank p95 request latency over all scored cases, output tokens per
second (completion tokens / request time), sampled maximum RSS and GPU
allocation. Rows are flagged **INCOMPLETE** when cases or the run failed,
and **STALE** when the suite changed since the run.

Latency depends on the task mix and output length. It is a like-for-like
comparison between models on the same host and backend, not a throughput
figure. Memory is the maximum of what `/app/status` reports for `geistd`,
polled every second (`memory.process_rss_bytes`; the app refreshes at most
every 2 s), so a short peak can be missed. RSS leaves out GPU buffers. GPU
allocation is shown separately where the platform reports it, and never added
to RSS (unified memory would double-count). Values that cannot be measured are
shown as `–`, never as zero.

## Tests

`python3 workbench/test_bench.py` (in CI, no model) checks that the builder
reproduces the frozen suite, the case counts, every scoring rule and the
report on synthetic runs.

## Reference results in the catalog

Measured 2026-10-03 on an Apple M1 Max (64 GB), Metal, engine pin
`33db79d7764b`; raw results in `workbench/evidence/<suite id>/`.

| Model | Correct | Note |
|---|---|---|
| Gemma 4 E4B · Q4_K_M | 147/160 | |
| Gemma 4 E2B · Q4_K_M | 146/160 | |
| Qwen3 0.6B · Q8_0 | 100/160 | reproduced exactly on a second run |
| Qwen3.5 0.8B · Q8_0 | 34/160 | at temperature 0, 110 cases think through the 3072-token budget without answering |
| SmolLM2 360M · Q8_0 | 31/160 | |
| BitNet b1.58 2B · I2_S (CPU) | 63/160 | after the template fix in #106; measured 2026-10-04 |

Not measured: Bonsai 2 27B and Qwen3.8 27B (too large for this round).
