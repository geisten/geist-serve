# Model loading and owned-process lifetime

An installed file and a ready model are different states. A private verification
receipt validates an unchanged file without hashing its contents again. The
receipt binds expected digest, inode/device, size, owner, permissions, links and
nanosecond modification/change timestamps. A changed or replaced file is checked
again. Invalid data cannot become a runtime. This is not a network download.

Selecting the identical ready model now returns 200, preserving the process and
readiness after validating that receipt. Auto/CPU/GPU policy changes that resolve
to the current backend already preserve the process. An effective backend change
uses a new generation: the app stops and reaps its old child before spawning the
replacement. It never stops foreign processes or keeps two resident models to
conceal a slow switch. Stop acknowledgement and UI polling remain asynchronous.

## Measured stages

The daemon writes numeric phase boundaries into a private, immediately unlinked
shared-memory file inherited as fd 4. Parent and child validate ABI/magic,
permissions, owner, generation and PID. Bounded lock-free snapshots contain no
model content, text or filesystem paths. This channel remains readable while
model initialization blocks the inference socket.

`/app/status.lifecycle` includes receipt hit/miss, bytes actually hashed on that
selection, previous PID, reap/spawn timestamps, and backend/model/metadata/warmup/
ready boundaries. `/app/status.activity` supplies selection, verification, stop,
request-open/session, tokenization, prefill and first visible-answer boundaries.
A no-op retains historical initialization phases; it does not claim to initialize
or hash the file again. Use the operation/generation/PID when comparing events.

The Bonsai baseline identifies `geist_model_load` as the dominant Metal load
stage. That call is an aggregate: tensor conversion, file faults, pipeline setup
and alias/copy work have no independent hook in this measurement. Physical disk
reads and an alleged “GPU upload” cannot be inferred from file size. The separate
request-open/session and prefill costs are not model-load time.

## Scoped optimization decisions

- Avoiding an identical active-model reload removes actual unnecessary work.
  Matched five-trial results include time to the first visible answer, not only
  readiness. See the derived evidence report and its retained raw SHA manifest.
- Keep the vocabulary-discovery forward pass for this change. The public API
  exposes a valid logits dimension after prefill; it does not expose an equivalent
  architecture-independent prefill-free dimension. Tokenizer length is not a
  verified replacement for padded/logits vocabulary size. Model load dominates
  the measured large Metal case; removing warmup without that contract would risk
  invalid token bounds and could just transfer initialization to the first answer.
- Keep the 2-thread Mac / 4-thread Pi cap unchanged. No thread-count experiment
  supports a new default here, and changing it would confound this loader result.
- Keep model processes isolated. Reusing immutable pipelines/buffers across
  different backend lifetimes or adding a persistent multi-model cache requires
  separate ownership and pressure analysis. This change makes no such reuse claim.
- Retain the bounded 120-second load deadline. Measured accepted stages are below
  it; a slow but progressing long prefill has its separate request deadline.
  The UI names the observed phase; it never fabricates percent progress.

## Cleanup and diagnostics

Exit/reap before new spawn and disappearance of owned sockets are release oracles.
Exact free-RAM restoration is not: reclaimable pages, driver accounting and other
applications may change system availability. Ten small-model and three Bonsai
CPU/Metal cycles retain numeric memory trends under predeclared noise budgets.
RSS remains process RSS, not a CPU+GPU total; [supported Metal telemetry](MEMORY-TELEMETRY.md)
adds a separately scoped allocation counter.

Failed CPU starts, crashes and cancelled loads preserve logs in distinct
`load-failure-*` files before another attempt truncates `server.log`. GPU failures
use distinct `gpu-failure-*` files before the CPU fallback. Synthetic tests cover
normal TERM, ignored TERM with bounded KILL/reap, spawn failure, load/ready crash,
cancellation, fallback and an untouched foreign PID. Diagnostics contain daemon
logs; exported performance/activity records remain numeric and contain no prompts.

## Reproduce

Run `make -f App.mk test-app` for deterministic/sanitizer contracts. For existing
local model files (no automatic 27B download), opt in to
`tests/app/lifecycle_real_test.py` using `GEIST_LIFECYCLE_MODELS`,
`GEIST_LIFECYCLE_MODEL_DIR` and a **new** `GEIST_LIFECYCLE_EVIDENCE` directory.
`GEIST_LIFECYCLE_REPS` defaults to five. Build the development daemon through
`GEISTD_OUTPUT=build/geistd-execution`, never overwrite a user's daemon.

Compare complete runs using `scripts/report-lifecycle.py BASELINE CANDIDATE`.
Reports do not modify raw observations. Trials with failures are retained in their
own directories; missing hardware is not a pass. Native Ubuntu CPU CI does not
certify Linux GPU or physical Pi behavior.
