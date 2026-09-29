#!/usr/bin/env python3
"""Opt-in Bonsai acceptance using the bundled catalog and an existing GGUF.

Never downloads a fixture. Set GEIST_BONSAI_GGUF_PATH and GEIST_EXECUTION_DAEMON.
Optional GEIST_BONSAI_EVIDENCE retains redacted observations and daemon logs.
"""
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from http_test import App, ROOT

fixture = os.environ.get('GEIST_BONSAI_GGUF_PATH')
if not fixture:
    print('Bonsai inference: SKIPPED (set GEIST_BONSAI_GGUF_PATH to an existing artifact)')
    raise SystemExit()
server = Path(os.environ.get('GEIST_EXECUTION_DAEMON', ROOT/'build/geistd-execution'))
binary = Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT/'geist-app'))
catalog = json.loads((ROOT/'models/catalog.json').read_text())
entry = next(m for m in catalog['models'] if m['id'] == 'bonsai2-27b-pq2')
assert 'unsupported_format' not in entry and entry['backends'] == ['cpu','metal']
fixture = Path(fixture).resolve(strict=True)
assert fixture.stat().st_size == entry['bytes']
with fixture.open('rb') as source:
    assert hashlib.file_digest(source,'sha256').hexdigest() == entry['sha256']
evidence = Path(os.environ['GEIST_BONSAI_EVIDENCE']) if os.environ.get('GEIST_BONSAI_EVIDENCE') else None
if evidence:
    evidence.mkdir(parents=True, exist_ok=False)

def save(name, data):
    if evidence:
        (evidence/name).write_text(json.dumps(data,indent=2)+'\n')

with tempfile.TemporaryDirectory(prefix='geist-bonsai-') as temporary:
    home = Path(temporary); (home/'models').mkdir()
    target = home/'models'/entry['file']
    if os.uname().sysname == 'Darwin':
        subprocess.run(['cp','-c',str(fixture),str(target)],check=True)
    else:
        shutil.copyfile(fixture,target)
    identity = (target.stat().st_ino,target.stat().st_mtime_ns)
    app = App(home,binary=binary,server=server)
    observations = []
    try:
        state = app.status()
        assert state['catalog_revision'] == catalog['revision']
        assert len(state['models']) == len(catalog['models']) == 9
        assert not (home/'catalog.json').exists(), 'must exercise the bundled catalog'
        model = next(m for m in state['models'] if m['id'] == entry['id'])
        assert model['installed'] and model['resource_fit'] != 2, model
        start = time.monotonic()
        assert app.request('/app/select',{'id':entry['id']})[0] == 202
        state = app.wait(lambda s:s['ready'],timeout=180)
        print(f'Bonsai initial load including integrity check: {time.monotonic()-start:.2f} s',flush=True)
        modes = ['cpu','gpu'] if state['execution']['gpu_available'] else ['cpu']
        for mode in modes:
            assert app.request('/app/execution',{'mode':mode})[0] in [200,202]
            state = app.wait(lambda s:s['ready'],timeout=180)
            assert state['active_id'] == entry['id']
            assert state['execution']['active'] == mode and state['execution']['verified']
            connection = http.client.HTTPConnection('127.0.0.1',app.port,timeout=180)
            body = {'experimental':True,'prompt':'Reply with exactly OK.',
                    'messages':[{'role':'user','content':'Reply with exactly OK.'}],
                    'model':entry['id'],'max_tokens':256}
            connection.request('POST','/app/generate',json.dumps(body),{'Authorization':'Bearer '+app.token})
            response = connection.getresponse()
            raw = response.read(); connection.close()
            events = [json.loads(line) for line in raw.splitlines()]
            save(f'{mode}-events.json',events)
            assert response.status == 200 and events[-1].get('done') and events[-1].get('eval_count',0)>0,raw
            answer=''.join(item.get('response','') for item in events)
            assert answer.strip() and '<think>' not in answer and '</think>' not in answer,raw
            assert events[-1]['reasoning'] and not events[-1]['no_answer'],events[-1]
            state = app.wait(lambda s:not s['busy'])
            history = {m['processor']:m for m in state['performance_history']}
            assert history[mode]['backend'] == state['execution']['backend']
            assert history[mode]['tokens'] == events[-1]['eval_count'] and history[mode]['rate'] > 0
            record = {'mode':mode,'execution':state['execution'],'resources':state['resources'],
                      'hardware':state['hardware'],'performance_history':state['performance_history'],
                      'stats':events[-1]}
            observations.append(record); save('observations.json',observations)
            print(json.dumps(record),flush=True)
        # The product's real connection probe must reach an answer beyond hidden preparation.
        env={**os.environ,'GEIST_HOME':str(home)}
        probe=subprocess.run([str(ROOT/'geist'),'test'],env=env,capture_output=True,text=True,timeout=180)
        assert probe.returncode==0,probe.stderr
        result=json.loads(probe.stdout)
        assert result['choices'][0]['message']['content'].strip() and '<think>' not in probe.stdout
        save('cli-probe.json',result)
        # Probe is another measurement; compare against the latest persisted history on restart.
        state=app.wait(lambda s:not s['busy'])
        history={m['processor']:m for m in state['performance_history']}
        assert set(history) == set(modes)
        assert (target.stat().st_ino,target.stat().st_mtime_ns) == identity
        assert not list((home/'models').glob('*.part'))
    finally:
        try: app.close()
        finally:
            if evidence and (home/'server.log').exists():
                (evidence/'daemon.log').write_bytes((home/'server.log').read_bytes())
    app = App(home,binary=binary,server=server)
    try:
        restored = app.wait(lambda s:s['ready'],timeout=180)
        assert restored['active_id'] == entry['id'] and restored['execution']['active'] == modes[-1]
        assert restored['execution']['verified']
        assert {m['processor']:m for m in restored['performance_history']} == history
        assert (target.stat().st_ino,target.stat().st_mtime_ns) == identity
    finally: app.close()
print('Bonsai: bundled catalog, real inference, verified processors, persisted metrics/restart and unchanged model file passed')
