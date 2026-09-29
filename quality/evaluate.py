#!/usr/bin/env python3
"""Run the actual C23 app; save exclusive, hash-bound outputs and blinded review."""
import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from evidence import POLICY, ROOT, sha, source_hash, summarize
sys.path.insert(0, str(ROOT / 'tests/app'))
from http_test import App


def automatic(case, output, error=None, limited=False):
    # Spelling a quantity out does not change its value. This narrow check is
    # deliberately not a substitute for the human factual/meaning review.
    words = {str(n): pair.split('/') for n, pair in enumerate([
        'zero/null','one/eins','two/zwei','three/drei','four/vier','five/fünf',
        'six/sechs','seven/sieben','eight/acht','nine/neun','ten/zehn',
        'eleven/elf','twelve/zwölf','thirteen/dreizehn','fourteen/vierzehn',
        'fifteen/fünfzehn','sixteen/sechzehn','seventeen/siebzehn',
        'eighteen/achtzehn','nineteen/neunzehn','twenty/zwanzig',
        'twenty-one/einundzwanzig','twenty-two/zweiundzwanzig','twenty-three/dreiundzwanzig'])}
    present = all(any(re.search(r'(?<!\w)' + re.escape(value) + r'(?!\w)', output, re.I)
                      for value in [item, *words.get(item, [])]) for item in case['required'])
    reasons = []
    if error: reasons.append('runtime_error')
    if not output.strip(): reasons.append('empty')
    if limited: reasons.append('output_limit')
    if not present: reasons.append('missing_required_fact')
    if any(marker in output for marker in ('<|im_end|>', '<|eot_id|>', '<think>')): reasons.append('protocol_or_reasoning_marker')
    if case['task'] == 'ideas':
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        listed = [re.sub(r'^(?:[-*]|\d+[.)])\s+', '', line) for line in lines if re.match(r'^(?:[-*]|\d+[.)])\s+', line)]
        items = listed if listed else [item.strip() for item in re.split(r'(?<=[.!?])\s+', output.strip()) if item.strip()]
        if len(items) != 3 or len(set(item.casefold() for item in items)) != 3: reasons.append('three_distinct_items_required')
    return {'automatic_pass': not reasons, 'automatic_reasons': reasons, 'critical': False}


def request(app, case):
    connection = http.client.HTTPConnection('127.0.0.1', app.port, timeout=180)
    data = dict(prompt=case['prompt'], task=case['task'], task_version='1.0.0', language=case['language'], experimental=True)
    start = time.monotonic()
    try:
        connection.request('POST', '/app/generate', json.dumps(data), {'Authorization': 'Bearer '+app.token})
        response = connection.getresponse()
        body = response.read(262145)
        if response.status != 200 or len(body) > 262144:
            raise ValueError(f'HTTP {response.status}: {body[:256]!r}')
        events = [json.loads(line) for line in body.splitlines() if line]
        if not events or not events[-1].get('done') or any(e.get('error') for e in events):
            raise ValueError('Incomplete/error stream')
        return ''.join(e.get('response', '') for e in events), events[-1], time.monotonic()-start
    finally:
        connection.close()


def review_page(run, directory):
    # Model/hardware identity withheld from the review page. No network dependencies.
    cells = summarize(run, {})
    selected = {x for cell in cells for x in cell['sample_ids']}
    cases = {c['id']: c for c in json.loads((directory/'corpus.json').read_text())['cases']}
    cases.update({c['id']:c for c in json.loads((directory/'adverse.json').read_text())['cases']})
    rows = [{'id':r['id'], 'task':r['task'], 'language':r['language'], 'prompt':cases[r['id']]['prompt'], 'answer':r['output']}
            for r in run['rows'] if r['id'] in selected]
    payload = json.dumps(rows, ensure_ascii=True).replace('<', '\\u003c')
    template = (ROOT/'quality/review.html').read_text().replace('/*CASES*/[]', payload)
    (directory/'review.html').write_text(template)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--model-id', required=True)
    p.add_argument('--expected-sha256', required=True)
    p.add_argument('--device', choices=['apple-silicon', 'pi5'], required=True)
    p.add_argument('--split', choices=['screening', 'acceptance'], required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if sha(a.model) != a.expected_sha256: raise ValueError('Model hash does not match catalog')
    a.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(ROOT/'quality/corpus.json', a.output/'corpus.json')
    shutil.copyfile(ROOT/'quality/adverse.json', a.output/'adverse.json')
    corpus = json.loads((a.output/'corpus.json').read_text())
    if a.split == 'acceptance':
        corpus['cases'] += json.loads((a.output/'adverse.json').read_text())['cases']
    run = dict(schema=1, model_id=a.model_id, model_sha256=a.expected_sha256, device=a.device, split=a.split,
               started_utc=datetime.now(timezone.utc).isoformat(),
               hardware={'system':platform.system(),'machine':platform.machine(),'processor':platform.processor(),
                         'uname':list(platform.uname())},
               configuration=POLICY['configuration'], source_sha256=source_hash(), policy_sha256=sha(ROOT/'quality/policy.json'),
               corpus_sha256=sha(a.output/'corpus.json'), app_sha256=sha(ROOT/'geist-app'), daemon_sha256=sha(ROOT/'geistd'),
               scorer_sha256=sha(Path(__file__)),
               adverse_sha256=sha(a.output/'adverse.json'),
               engine_ref=subprocess.check_output(['git','-C',str(ROOT/'geistlib'),'rev-parse','HEAD'],text=True).strip(),
               task_versions={t['id']:t['version'] for t in json.loads((ROOT/'tasks/catalog.json').read_text())['tasks']}, rows=[])
    (a.output/'provenance.json').write_text(json.dumps(run, indent=2)+'\n')
    with tempfile.TemporaryDirectory(prefix='geist-quality-') as home:
        app = App(home, model=a.model.resolve())
        try:
            app.wait(lambda state:state['ready'], timeout=240)
            with (a.output/'responses.jsonl').open('x') as raw:
                for case in corpus['cases']:
                    if case['split'] != a.split and not (a.split == 'acceptance' and case['split'] == 'adverse'): continue
                    error = None
                    try:
                        output, stats, elapsed = request(app, case)
                    except Exception as exc:
                        output, stats, elapsed, error = '', {}, None, str(exc)
                    row = dict(id=case['id'], task=case['task'], language=case['language'], adverse=case['split']=='adverse', output=output,
                               error=error, stats=stats, elapsed_s=elapsed,
                               **automatic(case, output, error, stats.get('limited', False)))
                    run['rows'].append(row)
                    raw.write(json.dumps(row, ensure_ascii=False)+'\n'); raw.flush()
                    print(f'{a.model_id} {case["id"]}: {"automatic-pass" if row["automatic_pass"] else row["automatic_reasons"]}', flush=True)
        finally:
            app.close()
    for name, value in [('run.json',run),('reviews.json',{}),('summary.json',summarize(run, {}))]:
        with (a.output/name).open('x') as stream: json.dump(value,stream,ensure_ascii=False,indent=2);stream.write('\n')
    review_page(run,a.output)
    (a.output/'manifest.json').write_text(json.dumps({'sha256':{n:sha(a.output/n) for n in ('run.json','reviews.json','corpus.json','adverse.json','responses.jsonl')}},indent=2)+'\n')


if __name__ == '__main__':
    main()
