#!/usr/bin/env python3
"""Opt-in real engine verification, including matched explicit comparison.
No download; use a known local SmolLM2 reference artifact.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from http_test import App, ROOT
model=os.environ.get('GEIST_TEST_MODEL')
if not model:
    print('performance real: SKIPPED (GEIST_TEST_MODEL not set)');raise SystemExit()
with tempfile.TemporaryDirectory(prefix='geist-performance-real-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    shutil.copyfile(model,home/'models/smollm2-360m-instruct-q8_0.gguf')
    (home/'selected').write_text('smollm2-360m')
    app=App(home,binary=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'build/geist-app-test')),server=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution')).resolve())
    try:
        state=app.wait(lambda s:s['ready'] and not s['busy'],timeout=90)
        daemon=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution'))
        build=json.loads(subprocess.check_output([str(daemon.resolve()),'--build-info']))
        assert state['engine']['geistlib']==build['geistlib']
        for mode in ('cpu','gpu'):
            if mode=='gpu' and not app.status()['execution']['gpu_available']:continue
            assert app.request('/app/execution',{'mode':mode})[0] in (200,202)
            app.wait(lambda s:s['ready'],timeout=90)
            for api in (False,True):
                body={'model':'smollm2-360m','messages':[{'role':'user','content':'Say hello in one short sentence.'}],'max_tokens':32,'temperature':0}
                if not api:body.update(prompt=body['messages'][0]['content'],experimental=True)
                code,data,_=app.request('/v1/chat/completions' if api else '/app/generate',body);assert code==200,data
                state=app.wait(lambda s:not s['busy'])
                print(json.dumps({'stage':'ordinary','mode':mode,'api':api,'history':state['performance_history']},ensure_ascii=False),flush=True)
        before=app.status()['execution']['mode']
        assert app.request('/app/performance/compare',{'confirm':True})[0]==202
        state=app.wait(lambda s:not s['comparison']['running'],timeout=600)
        assert state['comparison']['result']=='completed' and state['ready'] and state['execution']['mode']==before,state['comparison']
        code,data,_=app.request('/app/performance/export');assert code==200
        rows=[json.loads(line) for line in data.splitlines()]
        assert all(r['schema']==2 and r['engine']['geistlib']==build['geistlib'] and len(r['engine']['payload_sha256'])==64 for r in rows)
        for r in rows:
            memory=r['memory']
            assert memory['process_rss_bytes']==r['rss'] and memory['total_unique_physical_bytes'] is None
            assert memory['rss_source']==('macos.proc_pid_rusage.ri_resident_size' if os.uname().sysname=='Darwin' else 'linux.proc_pid_stat.rss')
            assert memory['status']==(1 if r['backend']=='metal' else 2), memory
            if r['backend']=='metal':assert memory['gpu_allocated_bytes']>0 and memory['gpu_samples']>0
            else:assert memory['gpu_allocated_bytes'] is None
        runs=[r for r in rows if r['source']=='controlled_test']
        assert len(runs)==(8 if state['execution']['gpu_available'] else 4)
        assert all(r['outcome']=='completed' and r['output']>0 and r['generation_ns']>0 and r['first_ns']>0 for r in runs)
        assert sum(r['warmup'] for r in runs)==len(runs)//4
        assert not any(r['contention'] for r in runs)
        print(json.dumps({'stage':'controlled','result':state['comparison'],'profile':state['performance_profile'],'records':runs},ensure_ascii=False),flush=True)
        evidence=os.environ.get('GEIST_PERFORMANCE_EVIDENCE')
        if evidence:
            target=Path(evidence);target.mkdir(parents=True,exist_ok=True)
            (target/'real-observations.jsonl').write_bytes(data)
            (target/'real-profile.json').write_text(json.dumps(state['performance_profile'],indent=2)+'\n')
    finally:app.close()
    app=App(home,binary=ROOT/'geist-app',server=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution')).resolve())
    try:
        state=app.wait(lambda s:s['ready'] and not s['busy'],timeout=90)
        code,data,_=app.request('/app/performance/export');restored=[json.loads(line) for line in data.splitlines()]
        assert {r['id'] for r in restored}=={r['id'] for r in rows}
        assert state['performance_profile']['cpu'] is not None
        assert state['execution']['mode']==before
    finally:app.close()
print('performance real: CPU/GPU app + editor, warmup/3 repetitions, processor restoration and restart passed')
