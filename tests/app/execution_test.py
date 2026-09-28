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
with tempfile.TemporaryDirectory(prefix='geist-execution-empty-') as home:
    app=App(home)
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
    app=App(home,server=server)
    try:
        assert app.request('/app/select',{'id':'smollm2-360m'})[0]==202
        state=app.wait(lambda s:s['ready'],timeout=90)
        assert state['execution']['active']=='cpu' and state['execution']['recommended']=='cpu'
        assert app.request('/app/execution',{'mode':'cpu'})[0]==200
        modes=['cpu','gpu','cpu'] if state['execution']['gpu_available'] else ['cpu']
        if len(modes)==1: assert app.request('/app/execution',{'mode':'gpu'})[0]==409
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
    finally: app.close()
    app=App(home,server=server)
    try:
        state=app.wait(lambda s:s['ready'],timeout=90)
        assert state['execution']['mode']=='cpu' and state['catalog_revision']==catalog['revision']
    finally: app.close()
print('execution: real inference, verified backend, CPU/GPU switching where available, no re-download, busy protection, active-catalog protection and restart preference passed')

# A fault wrapper only changes the GPU start; CPU inference stays real.
import shlex
with tempfile.TemporaryDirectory(prefix='geist-gpu-failure-') as temporary:
    home=Path(temporary)/'data';(home/'models').mkdir(parents=True)
    shutil.copyfile(fixture,home/'models/smollm2-360m-instruct-q8_0.gguf')
    wrapper=Path(temporary)/'daemon-fault.sh'
    wrapper.write_text("#!/bin/sh\nif [ \"$1\" = --backends ]; then printf '%s\\n' '{\"cpu\":{\"name\":\"cpu_neon\",\"available\":true},\"gpu\":{\"name\":\"metal\",\"available\":true}}'; exit 0; fi\nif [ \"$GEIST_BACKEND\" = metal ]; then echo GPU_START_FAULT >&2; exit 42; fi\nexec "+shlex.quote(str(server.resolve()))+' "$@"\n')
    wrapper.chmod(0o700)
    app=App(home,server=wrapper)
    try:
        assert app.request('/app/select',{'id':'smollm2-360m'})[0]==202
        app.wait(lambda s:s['ready'],timeout=90)
        assert app.request('/app/execution',{'mode':'gpu'})[0]==202
        state=app.wait(lambda s:s['ready'] and s['execution']['notice'],timeout=90)
        assert state['execution']['active']=='cpu' and state['execution']['mode']=='cpu'
        logs=list(home.glob('gpu-failure-*'))
        assert len(logs)==1 and 'GPU_START_FAULT' in logs[0].read_text()
    finally: app.close()
print('execution: failed GPU launch restores CPU once and retains diagnostics')
