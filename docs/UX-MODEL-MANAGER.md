# Model manager UX review — 28 September 2026

## Primary flow

Choose a model in the persistent list → observe download progress in the same
row → inspect its active processor and short response test → connect a tool.
The window remains a model manager with a short test, not a full chat client.

The left pane owns all catalog choices, their download rings, suitability and
actual modality symbols. The white right pane owns the active model, processor,
measured resources and test composer. Settings owns languages, catalog import
and storage. There is no second performance page or duplicate execution control.

## Research applied

- [NN/G: Progressive Disclosure](https://www.nngroup.com/articles/progressive-disclosure/):
  keep frequent model actions visible; put the occasional catalog import in
  Settings and expanded performance details behind their metric row. Reduction
  should not hide available downloads.
- [W3C: Radio Group Pattern](https://www.w3.org/WAI/ARIA/apg/patterns/radio/):
  Auto/CPU/GPU are mutually exclusive choices. Native radio inputs provide
  checked/disabled semantics, Tab entry and arrow-key navigation, with visible
  focus. Recommendation and current selection are separate states.
- [NN/G: Visibility of System Status](https://www.nngroup.com/articles/visibility-system-status/):
  preserve the model row during download, display actual progress, distinguish
  unavailable processors and report failures next to the affected model.

These are applied design principles, not a claim that this interface has passed
human usability testing. A future short usability test should observe whether a
new user can download a model, explain the green ring, identify the active
processor, switch it, connect a tool and recover from a rejected catalog import.
Record task completion, hesitation and errors; do not measure success only by
fewer clicks.

## Important states

- Download absent, paused, transferring, validating and verified use different
  symbols. Accessible names and tooltips carry meaning beyond color.
- Selected processor: light raised segment. Default processor (by model file
  size, not measured): explicit small label.
- Model rows name the weight format (8-bit, 4-bit, Ternary (native)) and never
  rank answer quality: no task/model pair has quality evidence yet (#80). The
  list says so in one line. The platform default is a "Suggested start", a
  compatibility and memory choice, not a quality recommendation. The active backend is shown separately, including Metal.
- Busy: switching/import disabled and also rejected by the service.
- GPU failure: restore CPU once; retain diagnostics and display the fallback.
- Invalid import: retain current models and explain rejection in Settings.
- Small window/text zoom: list and test stack; transcript and long drafts scroll
  independently while the send control remains reachable.

The palette stays white, neutral gray, restrained blue for selection/focus and
green for confirmed download/readiness. System typography, compact headings and
consistent spacing avoid a separate visual style for each widget.

## Processor changes and retained measurements

CPU/GPU reloads keep the same transcript, composer and expanded metrics in place.
The fixed-size status dot becomes a rotating ring; reduced-motion users get a
static dashed ring. A polite status announces the transition. Sending and a
second processor change are disabled until readiness; drafting and reading stay
available. Failure preserves context and reports the existing CPU recovery path.

The metric row shows the last successful CPU and GPU generation rates together.
Expanded details compare service-observed first text, total time, output tokens,
post-reply process RAM and timestamp. These are independent replies, not a fair
benchmark or a new automatic recommendation. No fastest badge is inferred.

## Verdicts: good enough and fast enough on this computer (#103)

Every model gets one verdict, shown as a symbol; the words are its tooltip and
accessible name. A verdict never blocks a model: ✗ explains, it does not
disable "Start model".

| Symbol | Verdict (`verdict.value`) | When |
|---|---|---|
| ✓ | `good` | reference test ≥ 90 % correct, typical answer ≤ 10 s, memory fits |
| ◐ | `usable` | as ✓, but the answer takes 10–30 s (`slow`) or memory is tight (`tight_memory`) |
| ✗ | `not_recommended` | does not fit this computer (`unavailable`), < 90 % correct (`unreliable`) or > 30 s per answer (`too_slow`) |
| ? | `unknown` | no reference test (`quality_unknown`) or no speed measurement here (`speed_unknown`) |

Known problems come first: a model that is measured as unreliable is ✗ even
before its speed is known. Missing data is never "good".

**Thresholds and why.**
- *10 s* for a typical answer is about the limit at which people still wait
  attentively for a reply.
- *30 s* is where a reply becomes a task that people come back to later.
- *90 %* correct means that, on average, at most one answer in ten needs to be
  checked and corrected.
- The defaults are in `APP_LIMITS_DEFAULT` (`src/app/tasks.h`). `/app/status`
  reports the current values as `limits`.

**Typical answer.** About 150 words, which is 200 tokens:
`first token time + 200 / output rate`, from the last ordinary reply on each
processor with this engine build. The faster processor counts.

**Quality.** All tasks and both languages of the reference test (#102). The
current intent is *Questions & chat*.

**Status shape (per model).**
```json
"verdict": {"value": "good", "reason": "good", "processor": "gpu",
            "seconds": {"cpu": 9.6, "gpu": 3.2}, "passed": 146, "total": 160}
```
Missing values are `null`. At top level:
- `best_choice` is `{id, verdict, processor}` for the installed model with the
  best verdict, then the highest pass rate, then the fastest answer; `null`
  when no model is installed.
- `limits` reports the thresholds in use.

**Measure speed on demand.** An installed model with no speed figure shows a
stopwatch button. It runs the existing controlled comparison: each processor
loads once, warms up, then answers three short questions. Another installed
model is first started the normal, verified way. During the run, the same
button cancels it, and the card shows the progress. The newest reply,
ordinary or controlled, is the verdict's speed basis (`speed` in
`app_prefs`). The slow-reply warning still uses ordinary replies only (#81).
