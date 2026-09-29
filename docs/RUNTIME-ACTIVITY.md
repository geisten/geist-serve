# Runtime activity and cancellation

The selected model keeps one reserved activity lane beside the conversation.
It shows the actual stage, processor, phase elapsed time and Stop. Its Activity
sheet reports total elapsed time, last progress age, bounded phase durations,
engine identity and a numeric failure code. Existing answer footers are unchanged.

`/app/status.activity` separates load, request and background-download operations.
Each carries a service-instance ID, operation ID, process generation, monotonic
sequence and exactly one terminal outcome. A service heartbeat or a live PID does
not count as stage progress. During synchronous input processing there is no
fraction, ETA or guessed preparation; recognized hidden output changes the stage
to preparation without exposing its contents. Status older than six seconds is
explicitly stale. A CPU wait hint appears after 15 seconds without switching the
processor. UI timers do not repeat screen-reader announcements.

`POST /app/activity/cancel` requires the normal private app authentication and
`{ "instance": "…", "id": 1, "generation": 1 }` matching the active operation.
It acknowledges before termination. Synchronous inference cancellation reaps the
app-owned process and reloads it before another request can run. An unrelated
background download retains its progress. No request is replayed on reconnect.
An expired/replaced operation returns 409; a missing credential returns 403.

A single lifecycle monitor runs independently of HTTP admission and the WebView.
It samples cached state and bounded process health. Request operations retain the
600-second prefill, 120-second generation-idle and one-hour overall bounds.
Closing a sheet does not stop collection. HTTP workers and phase histories remain
bounded. Initial loading is deliberately coarse (runtime/model loading); fine
backend/model/warmup attribution is a separate lifecycle change in #39.

## Validation

- `make -f App.mk test-app`: sanitized reducer, blocked operation peers, 20 status
  and cancel attempts per phase, stale instance/generation, termination/recovery,
  download coexistence, privacy, context/deadline and persistence regressions.
- `tests/desktop/chat_checks.js`: actual native WebView; pending acknowledgment,
  separate ownership, stale snapshots, stable geometry/draft/selection, modal
  focus/Escape, DE/EN, reduced-motion behavior and 200% zoom.
- `tests/app/activity_real_test.py`: opt-in existing small/Bonsai artifacts,
  CPU/eligible Metal, short and approximately 1,000-token synthetic inputs.
  The evidence directory retains frozen executable hashes, numeric activity
  traces and terminal timings. It never downloads a model or modifies the
  installed application. Linux CPU and packaged hosts remain CI gates.

Live activity is a bounded status snapshot, not a permanent prompt log. Exported
performance records retain engine attribution; no prompt or hidden preparation
text is added to diagnostics.
