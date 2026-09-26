# Task quality and experimental use

A model that fits memory is not necessarily suitable for a task. The app
keeps resource advice, locally measured speed and task quality separate.
Unknown or failed task/model/language/device combinations require an explicit,
unchecked-by-default experimental-use choice. The HTTP boundary enforces the
same policy. A manually supplied model remains experimental.

Quality records are keyed by model SHA-256, task version, language and device
family. They describe the tested configuration; they do not establish quality
for all possible prompts or performance on every computer in that family.
English and German never share a pass result. Simple chat covers bounded
questions, clarification and supplied context, not general-purpose competence.
Home Assistant has its own integration and acceptance gates.

## Frozen acceptance contract

`quality/policy.json` defines the gates. `corpus.json` contains 80 screening
cases and 400 acceptance cases: 50 per task/language. These are ten authored
scenario families with parameterized variants, not 400 independent observations
from user traffic. `adverse.json` adds 40 separate checks for negation,
uncertainty, quoted instructions, Unicode, constraints and missing capabilities.

Each task/language cell needs at least 90% automatic success, at least 90%
success in ten preselected human-reviewed ordinary answers, and all five
adverse answers to pass both checks. Any observed critical meaning/factual
error fails the cell. Human review covers every adverse answer and a blinded
sample of ordinary answers. It cannot establish that unreviewed answers have
no semantic errors. Mechanical anchors and formatting checks are deliberately
narrow; token speed and nonempty output are never sufficient quality evidence.

## Run and review

Build the app and daemon from the same checkout. Model files must match the
catalog's SHA-256. Run all six catalog models sequentially for screening:

```sh
python3 quality/screen.py --models /path/to/catalog-files \
  --device apple-silicon --output build/screen-unique-id
python3 quality/evaluate.py --model /path/to/verified-model.gguf \
  --model-id gemma4-e2b --expected-sha256 '<catalog SHA-256>' \
  --device apple-silicon --split acceptance --output build/acceptance-unique-id
```

Use `pi5` on a Pi. Heavy Pi work must hold the shared board's exclusive lock.
The runner drives the actual C23 app and geistd. Every output, runtime error
and truncation is retained. Existing result directories cannot be overwritten.
Timing in these quality runs is diagnostic, not a controlled speed comparison.
Run on a fixed source checkout and do not change code while collecting evidence.

Open the generated `review.html` locally. A human reviewer assesses 120 answers
(80 ordinary and 40 adverse), records reasons, and downloads `reviews.json`.
Model identity is withheld from that page. Do not enter personal information.

```sh
python3 quality/import_review.py --run build/acceptance-unique-id \
  --reviews /path/to/reviews.json --reviewer '<reviewer attribution>' \
  --output quality/bundles/reviewed-unique-id
```

The importer verifies corpus, policy, scorer, production-source and raw-output
hashes, model/engine/task identity, exact case coverage and recomputed scores.
Missing reviews, partial runs and stale builds cannot pass. Hashes provide an
audit trail, not an independent attestation of who ran or reviewed a model.
Inspect any host-identifying metadata before publishing artifacts; retain
original raw evidence privately if a separately labeled redacted copy is needed.

Only reviewed bundles listed in `quality/registry.json` enter a build. Changing
the relevant production source invalidates older evidence. An empty registry
means every combination remains experimental, as it does in this implementation.
`make test-app` includes fail-closed scoring and evidence-tampering regressions.
