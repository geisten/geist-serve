"""Fail-closed task evidence. Screening, missing ratings and stale builds cannot pass."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
POLICY = json.loads((ROOT / 'quality/policy.json').read_text())


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_hash():
    digest = hashlib.sha256()
    paths = sorted([* (ROOT / 'src').rglob('*.c'), *(ROOT / 'src').rglob('*.h'),
                    ROOT / 'clients/geistd_client.h', ROOT / 'tasks/catalog.json', ROOT / 'Makefile', ROOT / 'App.mk'])
    for path in paths:
        digest.update(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes() + b'\0')
    return digest.hexdigest()


def summarize(run, reviews):
    if len({r['id'] for r in run['rows']}) != len(run['rows']):
        raise ValueError('Duplicate result IDs')
    cells = []
    for task in POLICY['tasks']:
        for language in POLICY['languages']:
            rows = [r for r in run['rows'] if r['task'] == task and r['language'] == language and not r.get('adverse')]
            adverse = [r for r in run['rows'] if r['task'] == task and r['language'] == language and r.get('adverse')]
            # Sampling order is committed before answers exist, not cherry-picked after scoring.
            sample = sorted(rows, key=lambda r: hashlib.sha256(r['id'].encode()).hexdigest())[:POLICY['human_sample_per_cell']] + adverse
            rated = [reviews.get(r['id']) for r in sample]
            complete = len(sample) == POLICY['human_sample_per_cell'] + POLICY['adverse_cases_per_cell'] and all(
                isinstance(x, dict) and type(x.get('pass')) is bool and type(x.get('critical')) is bool
                and isinstance(x.get('reason'), str) and x['reason'].strip() for x in rated)
            ordinary_ratings = rated[:POLICY['human_sample_per_cell']]
            human_ok = complete and sum(x['pass'] for x in ordinary_ratings) / len(ordinary_ratings) >= POLICY['minimum_pass_rate']
            critical = sum(bool(r.get('critical')) for r in rows + adverse)
            critical += sum(bool(x.get('critical')) for x in rated if isinstance(x, dict))
            rate = sum(r.get('automatic_pass') is True for r in rows) / len(rows) if rows else 0
            enough = len(rows) == POLICY['acceptance_cases_per_cell'] and run.get('split') == 'acceptance'
            adverse_ok = len(adverse)==POLICY['adverse_cases_per_cell'] and all(r.get('automatic_pass') is True for r in adverse)
            adverse_human_ok = complete and all(reviews[r['id']]['pass'] for r in adverse)
            passed = enough and rate >= POLICY['minimum_pass_rate'] and human_ok and adverse_ok and adverse_human_ok and critical == 0
            failed = rate < POLICY['minimum_pass_rate'] or critical or (adverse and not adverse_ok) or (complete and (not human_ok or not adverse_human_ok))
            status = 'passed' if passed else 'failed' if rows and failed else 'unverified'
            cells.append({'task': task, 'language': language, 'quality': status, 'cases': len(rows),
                          'automatic_pass_rate': rate, 'critical_errors': critical,
                          'adverse_cases':len(adverse), 'adverse_automatic_pass':adverse_ok,
                          'human_complete': complete, 'sample_ids': [r['id'] for r in sample]})
    return cells


def load_bundle(path):
    path = Path(path).resolve()
    if not path.is_relative_to(ROOT / 'quality'):
        raise ValueError('Evidence must be in the reviewed quality directory')
    manifest = json.loads((path / 'manifest.json').read_text())
    for name in ('run.json', 'reviews.json', 'corpus.json', 'adverse.json', 'responses.jsonl'):
        if sha(path / name) != manifest['sha256'][name]:
            raise ValueError('Evidence hash mismatch: ' + name)
    run = json.loads((path / 'run.json').read_text())
    corpus = json.loads((path / 'corpus.json').read_text())
    corpus['cases'] += json.loads((path / 'adverse.json').read_text())['cases']
    reviews = json.loads((path / 'reviews.json').read_text())
    if reviews and not str(manifest.get('human_reviewer', '')).strip():
        raise ValueError('Human review requires explicit reviewer attribution')
    from evaluate import automatic
    if run.get('scorer_sha256') != sha(ROOT/'quality/evaluate.py'):
        raise ValueError('Evidence belongs to another scorer version')
    for name, field in [('corpus.json','corpus_sha256'), ('adverse.json','adverse_sha256')]:
        if run.get(field) != sha(ROOT/'quality'/name) or sha(path/name) != sha(ROOT/'quality'/name):
            raise ValueError('Evidence corpus is not the current frozen corpus')
    pin = re.search(r'^GEIST_REF\s*\?=\s*([a-f0-9]{40})$', (ROOT/'Makefile').read_text(), re.M).group(1)
    models = re.findall(r'\{"([^"]+)",\s*"[^"]+",\s*"[^"]+".*?"([a-f0-9]{64})"',
                        (ROOT/'src/app/core.c').read_text().split('const struct app_model app_models')[1].split('\n};',1)[0], re.S)
    versions = {t['id']:t['version'] for t in json.loads((ROOT/'tasks/catalog.json').read_text())['tasks']}
    if (run.get('engine_ref') != pin or (run.get('model_id'),run.get('model_sha256')) not in models
        or run.get('device') not in ('apple-silicon','pi5') or run.get('task_versions') != versions):
        raise ValueError('Model, engine, device or task identity mismatch')
    if any(not re.fullmatch('[a-f0-9]{64}',run.get(k,'')) for k in ('app_sha256','daemon_sha256')):
        raise ValueError('Missing binary provenance')
    raw = [json.loads(line) for line in (path/'responses.jsonl').read_text().splitlines()]
    if raw != run['rows']:
        raise ValueError('Run does not match preserved raw responses')
    if run['source_sha256'] != source_hash() or run['policy_sha256'] != sha(ROOT / 'quality/policy.json'):
        raise ValueError('Evidence belongs to another source/policy version')
    if run['corpus_sha256'] != sha(path / 'corpus.json') or run['configuration'] != POLICY['configuration']:
        raise ValueError('Corpus/configuration mismatch')
    expected = {c['id']: c for c in corpus['cases'] if c['split'] == run['split'] or (run['split']=='acceptance' and c['split']=='adverse')}
    if set(expected) != {r['id'] for r in run['rows']}:
        raise ValueError('Missing or extra evaluation cases')
    for row in run['rows']:
        case = expected[row['id']]
        if (row['task'], row['language']) != (case['task'], case['language']):
            raise ValueError('Case identity mismatch')
        if row.get('adverse',False) != (case['split']=='adverse'):
            raise ValueError('Case split mismatch')
        checked = automatic(case, row['output'], row.get('error'), row.get('stats', {}).get('limited', False))
        if any(row.get(key) != value for key, value in checked.items()):
            raise ValueError('Recorded automatic score disagrees with raw answer')
    return run, summarize(run, reviews)
