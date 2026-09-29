#!/usr/bin/env python3
"""Opt-in real Bonsai CPU deadline regression. Uses existing files, never downloads.

GEIST_BONSAI_GGUF_PATH, GEIST_EXECUTION_DAEMON, GEIST_REASONING_EVIDENCE required.
Runs in a private app home, preserves only synthetic answers and numeric evidence.
"""
import hashlib,http.client,json,os,shutil,subprocess,tempfile,time
from pathlib import Path
from http_test import App,ROOT
source=os.environ.get('GEIST_BONSAI_GGUF_PATH')
if not source:print('real reasoning/CPU deadline test: SKIPPED (opt-in Bonsai fixture)');raise SystemExit()
evidence=Path(os.environ['GEIST_REASONING_EVIDENCE']);evidence.mkdir(parents=True,exist_ok=False)
model=next(x for x in json.loads((ROOT/'models/catalog.json').read_text())['models'] if x['id']=='bonsai2-27b-pq2')
source=Path(source)
with source.open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==model['sha256']
with tempfile.TemporaryDirectory(prefix='geist-reasoning-real-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    target=home/'models'/model['file']
    if os.uname().sysname=='Darwin':subprocess.run(['cp','-c',str(source),str(target)],check=True)
    else:shutil.copyfile(source,target)
    (home/'selected').write_text(model['id']);(home/f"backend-{model['sha256']}").write_text('cpu')
    app=App(home,server=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution')))
    observations=[]
    try:
        state=app.wait(lambda s:s['ready'],timeout=180)
        assert state['execution']['active']=='cpu'
        (evidence/'hardware.json').write_text(json.dumps({'hardware':state['hardware'],'execution':state['execution']},indent=2))
        prompts=[('long-cpu','Read the reference text, then reply with only OK.\nReference:\n'+('The room contains a blue chair.\n'*120)+'\nReply with only OK.'),
                 ('short-cpu','What is 2 + 2? Reply with only the digit.')]
        for name,prompt in prompts:
            conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=30)
            body={'experimental':True,'model':model['id'],'prompt':prompt,'messages':[{'role':'user','content':prompt}]}
            started=time.monotonic();conn.request('POST','/app/generate',json.dumps(body),{'Authorization':'Bearer '+app.token})
            response=conn.getresponse();assert response.status==200
            events=[]
            for line in response:
                if not line.strip():continue
                event=json.loads(line);assert not event.get('error'),event
                event['arrival_s']=round(time.monotonic()-started,3);events.append(event)
            conn.close()
            (evidence/f'{name}-events.json').write_text(json.dumps(events,ensure_ascii=False,indent=2))
            answer=''.join(x.get('response','') for x in events)
            assert events[-1].get('done') and not events[-1]['no_answer'] and answer.strip(),events[-1]
            assert events[-1]['reasoning'] and '<think>' not in answer and '</think>' not in answer
            if name=='long-cpu':
                assert events[-1]['first_model_text_ns']>120e9,events[-1]
                assert events[0]['arrival_s']<15,'keepalive must arrive before long prefill completes'
            app.wait(lambda s:not s['busy'])
            observations.append({'case':name,'answer':answer,'stats':events[-1]})
            (evidence/'observations.json').write_text(json.dumps(observations,indent=2))
            print(json.dumps(observations[-1]),flush=True)
        # A deliberately reasoning-only explicit client budget returns a diagnostic, not leaked text.
        conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=30)
        conn.request('POST','/app/generate',json.dumps(body|{'max_tokens':1}),{'Authorization':'Bearer '+app.token})
        response=conn.getresponse();events=[json.loads(x) for x in response.read().splitlines()];conn.close()
        assert events[-1]['no_answer'] and not ''.join(x.get('response','') for x in events).strip()
        (evidence/'reasoning-only.json').write_text(json.dumps(events,indent=2))
    finally:
        app.close()
        for source_file in ['server.log','performance/observations.jsonl']:
            path=home/source_file
            if path.exists():shutil.copyfile(path,evidence/path.name)
print('real Bonsai CPU: long prefill beyond 120 seconds, complete hidden-reasoning answers and reasoning-only cap passed')
