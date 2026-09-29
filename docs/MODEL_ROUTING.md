# Client-requested model loading

Status: design proposal. The current gateway does not load models in response
to chat requests. `GET /v1/models` advertises the loaded model only. A different
model ID receives 404; an unready service receives 503 and a busy model receives
429. Desktop selection and download are explicit manager operations.

## Recommended next step

Put demand loading in the C23 `geist-app` manager gateway, which owns the catalog,
resource checks, consent, downloads and the private geistd process. Keep geistlib
application-neutral. A client sends the stable catalog ID in `model`; the manager
can start a previously downloaded, verified and permitted model on demand.

- Only installed, catalogued models with their existing hash-bound preview consent
  are eligible. Missing files return an actionable error; chat never downloads a
  model, accepts a license or grants consent implicitly.
- Keep one model resident by default. Serialize the transition and generation
  under the manager's ownership; never evict a model serving another request.
  A busy response should be retryable. Do not alternate between editor requests
  without a defined bounded queue or rejection policy.
- Recheck CPU/RAM compatibility and file identity before loading. Distinguish
  not-installed, not-permitted, unsuitable, loading and failed states. Apply a
  bounded startup timeout and cancellation/shutdown handling.
- The UI must show which model is loading/running and that terminal/editor clients
  share it. Preserve the old working state where possible on a failed transition.
- Test two competing clients, cancellation during cold start, failed model files,
  low memory, retries, model eviction protection and shutdown. A single successful
  chat is insufficient evidence for automatic model switching.

The unified desktop catalog is implemented separately from this proposal. Clicking
a model name or icon explicitly downloads and starts it, or starts its installed
file. Progress, pause and resume remain in that row. The manager rechecks resources
and keeps preview consent bound to the catalog hash. Chat requests do not trigger
this action.
