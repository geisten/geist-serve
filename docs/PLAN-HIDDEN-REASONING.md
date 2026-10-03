# Hide model reasoning in the product interface

Status: implemented locally in 0.5.25, 29 September 2026. **Revised 3 October
2026 (#93):** the maintainer decided to show progress and an opt-in, collapsed
thinking view; see [answer handling](ANSWER-HANDLING.md). The no-disclosure rule
below is superseded; everything about copy, history and the editor API still holds. See
[answer handling](ANSWER-HANDLING.md) for the implemented contract and limits.
The current ChatML renderer does not pre-open reasoning, so no pre-opened profile
is enabled. Native and packaged acceptance evidence is maintained in the workspace review.

## Outcome

The short model test shows the answer, while a compact **Preparing answer…**
status represents the preceding reasoning phase. German: **Antwort wird
vorbereitet …**. Reuse the existing message-status area and Stop control. Do not
add a reasoning tab or disclosure to the default interface.

Hiding a reasoning block changes presentation and response handling; it does not
disable the model's reasoning computation, shorten generation or reduce its token
usage. Disabling reasoning would need a separate, verified model-specific control.

## Current evidence

- `src/app/main.c:proxy_emit` forwards decoded daemon text as NDJSON `response`.
- `completion_emit` forwards the same unclassified text into the editor API's
  streaming `delta.content` or nonstreaming `message.content`.
- `web/app.js:run` concatenates these chunks, paints them through `chatMarkdown`,
  and retains the same output in the next request's assistant history.
- `addTurn` copies `output.markdownSource`, so CSS hiding alone would leave the
  reasoning in copy output and subsequent conversation requests.
- `web/markdown.js` deliberately renders model HTML as literal text. Preserve
  this protection; enabling arbitrary HTML is not a fix for `<think>` markers.
- The inspected product stream exposes no distinct reasoning channel. Template
  family detection currently does not define reasoning markers or a pre-opened
  reasoning phase. Do not infer that protocol from a model name alone.

## Implementation boundary

Add a small reusable C23 output separator in the **geist-serve application
service**, proposed as `src/app/output.c` and `output.h`. Apply it after incremental
UTF-8 decoding and before the existing app and editor response serializers.
The same request-local parser must serve both product endpoints, including
streaming and nonstreaming responses. This keeps Desktop, Terminal and editor
answer content consistent without adding presentation policy to geistlib.

The daemon's raw generation and inference library remain the source of generation
statistics. Do not rewrite raw benchmark evidence or train the model to address
this display issue.

### Protocol selection

1. Prefer a trusted, explicitly typed runtime channel if it becomes available.
2. For the reported text protocol, recognize a structural leading
   `<think>…</think>` block using a validated model-output profile.
3. Declare supported profiles in the JSON model catalogue, for example an
   optional allowlisted `reasoning_format` enum. Older catalogues default to no
   stripping; validate imports and preserve existing artifact IDs. Populate the
   affected entries only after inspecting the exact template/runtime and testing
   their output. An absent marker must still allow ordinary answers through.
4. Support a pre-opened reasoning state only when the actual rendered assistant
   prefix establishes it. Never assume an opening tag that was not emitted or
   supplied by a verified template.

Unknown protocols must not use guesses based on English prose, headings or model
names. Recognizing additional markers requires its own profile and fixtures.
Literal `<think>` in code fences, inline code, escaped text or an ordinary answer
must remain readable. Marker handling is protocol parsing, not HTML processing.

### Incremental state machine

Use request-local states: `prefix`, `reasoning`, `answer`, `finished`.

- In `prefix`, retain only the bounded possible delimiter prefix (and bounded
  leading whitespace). Once the text cannot be a supported opener, release it as
  answer content. Do not delay a normal answer until generation has completed.
- In `reasoning`, discard the body as it arrives. Retain only the bounded suffix
  needed to detect a closing marker split across chunks. Emit a phase event once,
  with no reasoning text. Do not accumulate the full hidden block.
- At the closing marker, enter `answer` and immediately forward any remaining
  bytes in the same chunk. Subsequent answer content goes through the existing
  safe Markdown/MathML renderer unchanged.
- Repeated leading structural blocks, if supported by the verified profile, must
  be specified explicitly. An unexpected delimiter sequence must never cause a
  partially buffered reasoning prefix to be flushed into the answer.
- Reset parser state for every request, including cancellation and model changes.
  Keep total generation, time and byte limits effective for hidden output too.

For the app, add an additive NDJSON phase event such as
`{"phase":"preparing"}`; keep the existing `response` and terminal event fields.
For the editor API, return only answer text in the existing content fields. Preserve
role, finish reason, usage and `[DONE]` semantics, and test client idle timeouts
when reasoning lasts before the first content delta. Do not invent a portable
`reasoning_content` extension or expose the hidden text through an auxiliary field.

## Interface and conversation behaviour

| State | User sees | Behaviour |
| --- | --- | --- |
| Waiting / reasoning | Existing small status: Preparing answer… | Stop remains available; composer stays usable for drafts |
| Answer starts | The answer streams normally | Clear preparation status; retain scroll and keyboard focus |
| Completed | Answer and existing performance footer | Copy includes only the answer |
| Stopped before an answer | Stopped before an answer was produced | No empty assistant turn; preserve or restore the user's draft |
| End / token limit without an answer | No answer was produced; Retry is available | Keep reasoning hidden; do not record a successful nonempty answer |
| Interrupted after answer text | Existing marked partial answer | Preserve only that visible partial answer in session history |

Do not advertise the normal continuation action when only hidden reasoning was
produced: no valid visible assistant turn exists to continue from. A retry creates
a fresh request; it must not silently resume the discarded reasoning or duplicate
the user's message.

Copy, browser selection, accessibility output and app follow-up requests must
never include a recognized reasoning block. Keep raw reasoning out of persistent
logs, performance files and exports. Existing session-only answer history remains
session-only. Clear chat clears the associated phase state as well.

Use an accessible status region announced only on phase changes. Retain semantic
button labels, stable focus, reduced-motion support and the same DE/EN behaviour.
No layout jump, expanding reasoning panel or competing indicator in the model list.

## Performance measurements

- Keep engine-generated token counts, generation time, tokens/s and total elapsed
  time honest: hidden reasoning still consumes generation work. Do not subtract
  reasoning characters as if they were token counts.
- Keep the established footer; explain in its accessible description/tooltip
  that generated tokens include preparation when reasoning was detected.
- Distinguish existing time-to-first-model-text from time-to-first-visible-answer.
  Add an optional numeric first-answer field, leaving legacy first-text measurements
  unchanged. Do not silently reinterpret or combine historical measurements.
- A reasoning-only completion has generation statistics but no successful visible
  answer. Preserve that distinction in records and exclude it from successful
  answer aggregates; do not fabricate response quality or a zero answer latency.

## Work sequence

1. Confirm the affected model artifact and exact template with a reproducible local
   response. Record delimiters and rendered prefix, without copying user chat logs.
2. Specify the catalogue profile and stream contract; add parser fixtures for that
   protocol, including deliberately fragmented UTF-8 and marker boundaries.
3. Implement and test the bounded C23 separator. Integrate app/editor adapters,
   cancellation, finish states and numeric observation semantics together.
4. Adapt the existing message-status, final-answer copy/history and DE/EN strings.
   Preserve Markdown/math, per-answer metrics and background downloads.
5. Run adapter, native UI and real-model acceptance. Build an installer only after
   these checks pass; report untested platforms separately.

## Acceptance criteria and tests

1. The reported `<think>…</think>Answer` form displays and copies **only Answer**.
2. Test every possible chunk split through both markers, one byte per chunk,
   multibyte text, both markers plus an answer in one chunk, and multiple requests.
3. Inspect emitted events and DOM after **every chunk**: no transient flash of a
   recognized block, no reasoning in accessible output, clipboard or follow-ups.
4. Ordinary no-marker responses have the same text and incremental behaviour;
   code/escaped delimiter examples and Markdown/math remain intact.
5. Cover an empty block, a pre-opened profile, incomplete delimiters, missing closing
   tag, unsupported protocol, unexpected marker order and reasoning-only output.
   Verify precise final/error states and that hidden text is never flushed on EOF.
6. Cancellation, disconnect, token/byte/time limits and reset during reasoning
   terminate promptly, preserve the draft and never create an empty assistant turn.
7. A final answer can be continued and used in multi-turn requests without hidden
   text leaking back into the prompt. Test a real reasoning model and a plain model.
8. App NDJSON, editor SSE and nonstreaming JSON yield the same final answer;
   stream framing, finish reason, usage and connection tests remain compatible.
9. Reasoning length does not grow parser memory; repeated requests release their
   state. Run ASan/UBSan and oversized/invalid input fixtures.
10. Existing tokens/s remains based on all generated tokens. New first-answer
    latency and reasoning-only outcomes round-trip through storage without
    reclassifying old measurements or storing text.
11. Native UI tests cover DE/EN, keyboard/Stop, copy, clear, reduced motion, narrow
    and enlarged layouts, and chat while a different model downloads.
12. Catalogue imports retain backward compatibility, reject unknown profile values
    safely, and do not change downloaded files or verification receipts.

Limit: text delimiters cannot establish intent for every arbitrary model output.
The guarantee applies to declared and tested protocols; this is an output-format
feature, not a security boundary against a malicious model.
