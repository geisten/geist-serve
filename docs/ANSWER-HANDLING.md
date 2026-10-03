# Answer handling

The shared app service (not geistlib) separates declared reasoning protocols.
`output.c` receives validated UTF-8, discards hidden bodies incrementally and keeps
only a fixed delimiter buffer. App NDJSON, streaming editor SSE and nonstreaming
JSON contain the visible answer. Markdown HTML remains escaped. This is protocol
handling, not a security boundary against malicious or ambiguous model output.

While awaiting an answer the app shows what happens (#93): **Reading your
input · N tokens…** when input processing takes longer than a second (the event
commits the HTTP status, so a quick failure keeps its error code), then
**Thinking · m:ss · N tokens · X t/s**, from `{"phase":"preparing","tokens":N}`
about once a second. The reasoning text reaches the app endpoint only as
`{"thinking":"…"}` events, without markers, and is shown as plain text in a
collapsed **Show thinking** disclosure. The editor API (`/v1`) never receives it.
Reasoning never enters copy, accessible answer text, follow-up messages, logs,
measurement records or exports; it is session-only and Clear chat removes it. A missing close
marker or reasoning-only completion produces an explicit no-answer result and a
retry. A partial answer survives cancellation with an interruption label. A retry
never inserts an empty assistant message. The current renderer does not pre-open
reasoning; no pre-opened or guessed prose-based profile is supported.

## Budgets and cancellation

Normal app chat omits `max_tokens`. After tokenizing the complete session, the
service uses the remaining daemon context (currently 4,096 positions, including
input and terminal reserve). This replaces the former fixed 1,024-token cap and
manual continuation button. It does not enlarge the model's runtime context or
silently discard history. An actual context boundary is explained in the reply.
Editor clients retain their explicit upper bound (1–4,095); it is clamped to the
remaining context. The editor default remains 256 when unspecified.

Prefill has a ten-minute deadline. Generation has a 120-second frame-idle limit
and the entire request has a one-hour limit. NDJSON/SSE keepalives without hidden
content occur every ten seconds. Nonstreaming clients must configure their own
HTTP read timeout for long CPU requests. The five-second HTTP upload deadline is
unchanged. Cancellation is checked at most every 50 ms in client socket I/O.

Library prefill is synchronous. After its cancellation/failure, the app reaps
only its own daemon, archives its log as a distinct `request-failure-*` file and
reloads the same model/backend. The UI remains in loading state until ready.
If diagnostic archival fails, the daemon stays stopped instead of losing evidence.

## Errors and measurements

Errors identify input processing, generation or a disconnected model service
and appear on the affected reply in DE/EN. The authenticated `/app/status` includes
`request_phase` and `last_error` (message, stage, model, backend, code). The record
contains no prompt or model output, stays in memory, and clears on the next request.

The footer keeps all generated tokens and generation time, including hidden
preparation. `first_model_text_ns` retains its old meaning. New
`first_answer_ns` measures the first visible answer; unknown is -1 in terminal
NDJSON and null in the numeric history. Optional history fields `reasoning` and
`first_answer_ns` do not reinterpret existing records. `no_answer` observations
are retained diagnostically and excluded from successful-answer comparisons.
