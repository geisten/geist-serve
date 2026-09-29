#!/usr/bin/env python3
"""Opt-in lifecycle with two real quantizations of one checkpoint, no downloads.

GEIST_VARIANT_Q4 / GEIST_VARIANT_Q8 point to existing Qwen3.5 4B GGUFs.
Fixture catalog entries are test-only, never shipped as product recommendations.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from http_test import App, ROOT

paths = [os.environ.get('GEIST_VARIANT_Q4'), os.environ.get('GEIST_VARIANT_Q8')]
if not all(paths):
    print('variant inference: SKIPPED (set GEIST_VARIANT_Q4 and GEIST_VARIANT_Q8); catalog migration covered by catalog_test.py')
    raise SystemExit()
fixtures = [Path(p).resolve(strict=True) for p in paths]
binary = Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT/'build/geist-app-test'))
server = Path(os.environ.get('GEIST_EXECUTION_DAEMON', ROOT/'build/geistd-execution'))
catalog = json.loads((ROOT/'models/catalog.json').read_text())
catalog['revision'] += 1
models = []
for quant, fixture in zip(['Q4_0','Q8_0'], fixtures):
    with fixture.open('rb') as source: digest=hashlib.file_digest(source,'sha256').hexdigest()
    models.append({**catalog['models'][1], 'id':f'variant-fixture-{quant}',
                   'name':f'Qwen3.5 4B fixture {quant}', 'group_id':'qwen35-4b-fixture',
                   'group_name':'Qwen3.5 4B fixture', 'quantization':quant,
                   'file':f'fixture-{quant}.gguf', 'sha256':digest, 'bytes':fixture.stat().st_size,
                   'working_mib':6144, 'recommended_ram_gib':8})
assert models[0]['sha256'] != models[1]['sha256'], 'real distinct quantizations required'
catalog['models'] = models
with tempfile.TemporaryDirectory(prefix='geist-real-variants-') as temporary:
    home=Path(temporary); (home/'models').mkdir()
    for fixture, model in zip(fixtures,models):
        target=home/'models'/model['file']
        if os.uname().sysname=='Darwin': subprocess.run(['cp','-c',str(fixture),str(target)],check=True)
        else: shutil.copyfile(fixture,target)
    (home/'catalog.json').write_text(json.dumps(catalog))
    app=App(home,binary=binary,server=server)
    observed={}
    try:
        for model in models:
            assert app.request('/app/select',{'id':model['id']})[0]==202
            state=app.wait(lambda s:s['ready'] and not s['busy'] and not s['phase'] and s['active_id']==model['id'],timeout=120)
            assert state['active_id']==model['id']
            # First observation of one variant must not inherit its sibling's numbers.
            assert state['performance_history']==[]
            assert app.request('/app/execution',{'mode':'cpu'})[0] in (200,202)
            state=app.wait(lambda s:s['ready'] and not s['busy'] and not s['phase'] and s['active_id']==model['id'],timeout=120)
            assert state['execution']['active']=='cpu'
            status,raw,_=app.request('/app/generate',{'experimental':True,'model':model['id'],
                                  'prompt':'Say hello.','messages':[{'role':'user','content':'Say hello.'}],'max_tokens':16})
            events=[json.loads(line) for line in raw.splitlines()]
            assert status==200 and events[-1].get('done') and events[-1].get('eval_count',0)>0,raw
            assert ''.join(item.get('response','') for item in events).strip()
            state=app.wait(lambda s:not s['busy'])
            observed[model['id']]=state['performance_history']
            assert observed[model['id']][0]['rate']>0
            print(json.dumps({'variant':model['quantization'],'actual_backend':state['execution']['backend'],
                              'tokens':events[-1]['eval_count'],'history':state['performance_history']}),flush=True)
        first,second=models
        for model in [first,second]:
            assert app.request('/app/select',{'id':model['id']})[0]==202
            state=app.wait(lambda s:s['ready'] and not s['busy'] and not s['phase'] and s['active_id']==model['id'],timeout=120)
            assert state['performance_history']==observed[model['id']]
        log=os.pread(app.log.fileno(),1000000,0)
        assert log.count(b'model verification: hashing ')==2, 'A/B/A/B hashes each artifact just once'
        second_file=home/'models'/second['file']; before=(second_file.stat().st_ino,second_file.stat().st_mtime_ns)
        assert app.request('/app/remove',{'id':first['id']})[0]==200
        state=app.status()
        assert state['active_id']==second['id'] and state['ready']
        assert len(state['models'])==2 and len({m['group_id'] for m in state['models']})==1
        assert [m['installed'] for m in state['models']]==[False,True]
        assert not (home/'models'/first['file']).exists()
        assert before==(second_file.stat().st_ino,second_file.stat().st_mtime_ns)
        assert state['performance_history']==observed[second['id']]
    finally: app.close()
    app=App(home,binary=binary,server=server)
    try:
        state=app.wait(lambda s:s['ready'] and s['active_id']==second['id'],timeout=120)
        assert state['active_id']==second['id'] and state['performance_history']==observed[second['id']]
        assert [m['installed'] for m in state['models']]==[False,True]
        assert before==(second_file.stat().st_ino,second_file.stat().st_mtime_ns)
    finally: app.close()
print('variants: real Q4/Q8 inference, isolated observations, cached switching, sibling-safe deletion and restart passed')
