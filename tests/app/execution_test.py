#!/usr/bin/env python3
"""Processor selection with the actual packaged daemon and a local model fixture."""
import copy
import http.client
import json
import os
from pathlib import Path
import shutil
import tempfile
from http_test import App,ROOT

server=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'geistd'))
fixture=os.environ.get('GEIST_TEST_MODEL')
binary=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'geist-app'))
with tempfile.TemporaryDirectory(prefix='geist-execution-empty-') as home:
    app=App(home,binary=binary)
    try:
        assert app.request('/app/execution',{'mode':'gpu'},auth=False)[0]==403
        for mode in [None,[],42,'vulkan','']:
            assert app.request('/app/execution',{'mode':mode})[0]==400
        assert app.request('/app/execution',{'mode':'cpu'})[0]==409
    finally: app.close()
if not fixture:
    print('execution: model-free validation passed; real CPU/GPU test skipped (GEIST_TEST_MODEL not set)')
    raise SystemExit()
with tempfile.TemporaryDirectory(prefix='geist-execution-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    target=home/'models/smollm2-360m-instruct-q8_0.gguf';shutil.copyfile(fixture,target)
    model_stat=target.stat()
    app=App(home,binary=binary,server=server)
    try:
        assert app.request('/app/select',{'id':'smollm2-360m'})[0]==202
        state=app.wait(lambda s:s['ready'],timeout=90)
        assert state['execution']['active']=='cpu' and state['execution']['recommended']=='cpu'
        assert app.request('/app/execution',{'mode':'cpu'})[0]==200
        modes=['cpu','gpu','cpu'] if state['execution']['gpu_available'] else ['cpu']
        if len(modes)==1: assert app.request('/app/execution',{'mode':'gpu'})[0]==409
        observed = {}
        for mode in modes:
            assert app.request('/app/execution',{'mode':mode})[0] in [200,202]
            state=app.wait(lambda s:s['ready'],timeout=90)
            assert state['execution']['active']==mode, state['execution']
            if os.environ.get('GEIST_EXECUTION_DAEMON'): assert state['execution']['verified'],state['execution']
            body={'experimental':True,'prompt':'Say hello in one sentence.','messages':[{'role':'user','content':'Say hello in one sentence.'}],'model':'smollm2-360m','max_tokens':32}
            code,data,_=app.request('/app/generate',body)
            events=[json.loads(line) for line in data.splitlines()]
            assert code==200 and events[-1].get('done') and events[-1]['eval_count']>0, data
            reply=''.join(item.get('response','') for item in events)
            assert reply.strip(),data
            state = app.wait(lambda s: not s['busy'])
            history = {m['processor']:m for m in state['performance_history']}
            sample = history[mode]
            assert sample['backend'] == state['execution']['backend']
            assert sample['tokens'] == events[-1]['eval_count'] and sample['rate'] > 0
            performance = state['execution']['performance']
            assert performance['target_tps'] == 8
            if state['execution']['verified']:
                assert performance['rate'] == sample['rate']
                assert performance['below_target'] == (sample['rate'] < 8)
            assert 0 <= sample['first'] <= sample['total'] and sample['rss_bytes'] > 0
            for previous in observed:
                if previous != mode: assert history[previous] == observed[previous]
            observed[mode] = sample
            print(json.dumps({'mode':mode,'backend':state['execution']['backend'],'verified':state['execution']['verified'],'reply':reply,'stats':events[-1]},ensure_ascii=False),flush=True)
        assert (target.stat().st_ino,target.stat().st_mtime_ns)==(model_stat.st_ino,model_stat.st_mtime_ns)
        # The engine may not be replaced while either API is generating.
        conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=30)
        conn.request('POST','/app/generate',json.dumps({'experimental':True,'prompt':'List one hundred animal names.'}),{'Authorization':'Bearer '+app.token})
        response=conn.getresponse();assert response.status==200 and response.readline()
        assert app.request('/app/execution',{'mode':'auto'})[0]==409
        catalog=json.loads(app.request('/app/catalog')[1]);catalog['revision']+=1
        assert app.request('/app/catalog',catalog)[0]==409
        response.close();conn.close();app.wait(lambda s:not s['busy'])
        changed=copy.deepcopy(catalog);changed['models']=[m for m in changed['models'] if m['id']!='smollm2-360m']
        assert app.request('/app/catalog',changed)[0]==409
        assert app.request('/app/catalog',catalog)[0]==200
        assert app.status()['ready']
        assert {m['processor']:m for m in app.status()['performance_history']} == observed
        files = list((home/'performance').glob('*.jsonl'))
        assert files
        assert all((f.stat().st_mode & 0o777) == 0o600 for f in files)
        assert all('Say hello' not in f.read_text() and 'animal names' not in f.read_text() for f in files)
    finally: app.close()
    app=App(home,binary=binary,server=server)
    try:
        state=app.wait(lambda s:s['ready'],timeout=90)
        assert state['execution']['mode']=='cpu' and state['catalog_revision']==catalog['revision']
        assert {m['processor']:m for m in state['performance_history']} == observed
        if state['execution']['verified']:
            assert state['execution']['performance']['rate'] == observed['cpu']['rate']
    finally: app.close()
    # Detailed corruption/retention/engine migration checks live in performance_test.py.
print('execution: real processor switches, numeric histories and restart passed')
