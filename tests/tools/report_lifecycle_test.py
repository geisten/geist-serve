#!/usr/bin/env python3
"""scripts/report-lifecycle.py on two small synthetic evidence directories:
the table, the regression guard and the integrity section."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def evidence(root, ready, head):
    root.mkdir()
    (root/'build.json').write_text(json.dumps({'head': head, 'repetitions': 2}))
    (root/'complete.json').write_text(json.dumps({'passed': True, 'transitions': 4}))
    phases = [{'stage': s, 'duration_ms': d} for s, d in (('backend', 5), ('model', 400), ('metadata', 20), ('warmup', 90))]
    rows = [{'transition': f'{n}:load-small', 'ready_s': ready, 'first_answer_from_action_s': ready + .2, 'same_pid': False,
             'lifecycle': {'engine_phases': phases}} for n in range(2)]
    rows += [{'transition': f'{n}:same-model', 'ready_s': .01, 'first_answer_from_action_s': .1, 'same_pid': True,
              'lifecycle': {'engine_phases': phases}} for n in range(2)]
    rows.append({'transition': 'cleanup:stop', 'ready_s': 0, 'first_answer_from_action_s': 0, 'same_pid': False, 'lifecycle': {}})
    (root/'results.json').write_text(json.dumps(rows))
    (root/'samples.jsonl').write_text(''.join(json.dumps({'supervisor_rss_bytes': 30 << 20, 'available_bytes': 40 << 30}) + '\n' for _ in range(3)))


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    evidence(tmp/'base', 1.0, 'a' * 40)
    evidence(tmp/'same', 1.05, 'b' * 40)
    evidence(tmp/'slow', 2.0, 'c' * 40)
    run = lambda *dirs: subprocess.run([sys.executable, ROOT/'scripts/report-lifecycle.py', *dirs], capture_output=True, text=True, check=True).stdout
    out = run(tmp/'base', tmp/'same')
    assert 'Revisions aaaaaaa → bbbbbbb. 2 fixed-input trials' in out, out
    assert '| load-small | 1.000 → 1.050 | 1.200 → 1.250 | within guard |' in out, out
    assert '| load-small | 5.0 / 400.0 / 20.0 / 90.0 |' in out and 'same-model |' not in out.split('## Initialization')[1].split('## Cleanup')[0], out
    assert 'Guard findings: `[]`' in out and out.count('SHA-256 `') == 8, out
    out = run(tmp/'base', tmp/'slow')
    assert 'REVIEW: ready_s, first_answer_from_action_s' in out and '"regression"' in out, out
    (tmp/'slow'/'complete.json').write_text(json.dumps({'passed': False}))
    failed = subprocess.run([sys.executable, ROOT/'scripts/report-lifecycle.py', tmp/'base', tmp/'slow'], capture_output=True, text=True)
    assert failed.returncode != 0, 'a failed run is never reported as evidence'
print('report-lifecycle: comparison table, regression guard, attribution and integrity passed')
