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
- Selected processor: light raised segment. Recommended processor: explicit
  small label. The active backend is shown separately, including Metal.
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
