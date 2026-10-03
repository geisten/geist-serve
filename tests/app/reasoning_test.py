#!/usr/bin/env python3
"""Real C HTTP, profile selection, fragmentation, SSE, budget, diagnostics and recovery."""
import hashlib,http.client,json,os,socket,tempfile,time
from pathlib import Path
from http_test import App,ROOT
binary=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'build/geist-app-test'))
with tempfile.TemporaryDirectory(prefix='geist-reasoning-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    catalog=json.loads((ROOT/'models/catalog.json').read_text());catalog['revision']+=1
    model=catalog['models'][2];model.update(bytes=4,sha256=hashlib.sha256(b'test').hexdigest(),working_mib=1,recommended_ram_gib=1,backends=['cpu'])
    (home/'models'/model['file']).write_bytes(b'test')
    (home/'catalog.json').write_text(json.dumps(catalog));(home/'selected').write_text(model['id'])
    def config(**kw):(home/'fixture.json').write_text(json.dumps(kw))
    config()
    app=App(home,binary=binary,server=ROOT/'tests/app/reasoning_fixture.py')
    prompt={'model':model['id'],'messages':[{'role':'user','content':'Test'}],'prompt':'Test','experimental':True}
    def events(extra=None):
        code,raw,_=app.request('/app/generate',prompt|(extra or {}));assert code==200,(code,raw)
        e=[json.loads(x) for x in raw.splitlines()]
        # #93: the thinking text reaches the app only as "thinking" events, never as answer text.
        assert all('SECRET' not in x.get('response','') for x in e) and all(set(x)<={'thinking'} for x in e if 'thinking' in x),raw
        return e
    try:
        app.wait(lambda s:s['ready'])
        e=events();assert ''.join(x.get('response','') for x in e)=='**Answer** → 🌿'
        assert e[-1]['reasoning'] and not e[-1]['no_answer'] and e[-1]['eval_count']==50
        assert 0<=e[-1]['first_model_text_ns']<=e[-1]['first_answer_ns']
        assert int((home/'requested-max').read_text())==4083
        assert any(x.get('phase')=='preparing' for x in e)
        # #93: the thinking arrives in order, without its markers, and reports its token count.
        assert ''.join(x.get('thinking','') for x in e)=='SECRET' and all('<' not in x.get('thinking','') for x in e),e
        assert any(x.get('phase')=='preparing' and x.get('tokens',0)>=1 for x in e),e
        assert not any(x.get('phase')=='prefill' for x in e),'a quick input read is not announced (keeps error codes)'
        for stream in [False,True]:
            code,raw,_=app.request('/v1/chat/completions',prompt|{'stream':stream,'max_tokens':2000,'stream_options':{'include_usage':True}})
            assert code==200 and b'SECRET' not in raw and b'<think>' not in raw,raw
            assert (home/'requested-max').read_text()=='2000'
            if stream:
                chunks=[x[6:] for x in raw.decode().splitlines() if x.startswith('data: ')];assert chunks[-1]=='[DONE]'
                data=[json.loads(x) for x in chunks[:-1]]
                answer=''.join(c.get('delta',{}).get('content','') for x in data for c in x['choices']);assert answer=='**Answer** → 🌿'
                assert data[-1]['usage']['completion_tokens']==50
            else:assert json.loads(raw)['choices'][0]['message']['content']=='**Answer** → 🌿'
        for text in ['<think>SECRET','<thi','<think></think>','<think>SECRET</think>']:
            config(text=text);e=events();assert e[-1]['no_answer'] and not e[-1]['limited']
        config(text='Plain `code` with <think>literal</think>.');assert ''.join(x.get('response','') for x in events())=='Plain `code` with <think>literal</think>.'
        config(text='long answer',tokens=1500,reason='max');e=events();assert e[-1]['limited'] and e[-1]['eval_count']==1500
        config(fail='context');assert app.request('/app/generate',prompt)[0]==400
        config(input=4095);assert app.request('/app/generate',prompt)[0]==400
        for stage in ['generate','prefill']:
            config(fail=stage);code,raw,_=app.request('/app/generate',prompt);assert code==502,(code,raw)
            assert b'Check its status' not in raw and b'Model execution failed' in raw
            state=app.wait(lambda s:s['ready'] and not s['busy']);assert state['last_error']['stage']==stage and state['last_error']['model']==model['id']
        assert list(home.glob('request-failure-*')), 'prefill failure retains diagnostics'
        # Cancel a truly blocked synchronous prefill; reaping the owned child must free the service.
        config(prefill_pause=30)
        (home/'prefill-started').unlink(missing_ok=True)
        conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=15)
        conn.request('POST','/app/generate',json.dumps(prompt),{'Authorization':'Bearer '+app.token})
        deadline=time.monotonic()+5
        while not (home/'prefill-started').exists() and time.monotonic()<deadline:time.sleep(.02)
        assert app.status()['request_phase']=='prefill'
        conn.close();app.wait(lambda s:s['ready'] and not s['busy'],timeout=10)
        config();events()
        # Keepalive arrives before input processing completes, without exposing hidden text.
        config(prefill_pause=11)
        e=events();assert any(x.get('phase')=='preparing' for x in e)
        assert any(x.get('phase')=='prefill' and x.get('tokens',0)>0 for x in e),'#93: a long input read is announced with its size'
        assert next(i for i,x in enumerate(e) if x.get('heartbeat')) < next(i for i,x in enumerate(e) if x.get('phase')=='preparing'), 'prefill heartbeat does not guess preparation'
        config(prefill_pause=11,text='Plain answer')
        e=events();assert any(x.get('heartbeat') for x in e) and not any(x.get('phase')=='preparing' for x in e)
        time.sleep(.1)
    finally:app.close()
    rows=[json.loads(x) for x in (home/'performance/observations.jsonl').read_text().splitlines()]
    assert any(x['outcome']=='no_answer' for x in rows)
    assert any(x['reasoning'] and x['first_answer_ns'] is not None for x in rows)
    assert all('SECRET' not in json.dumps(x) and 'Answer' not in json.dumps(x) for x in rows)
    # A scaled monotonic deadline exercises the real timeout/error/recovery path.
    config(prefill_pause=2)
    app=App(home,binary=binary,server=ROOT/'tests/app/reasoning_fixture.py',env={**os.environ,'GEIST_TEST_PREFILL_MS':'100'})
    try:
        app.wait(lambda s:s['ready'])
        code,raw,_=app.request('/app/generate',prompt)
        assert code==504 and b'Input processing timed out' in raw,(code,raw)
        state=app.wait(lambda s:s['ready'] and not s['busy']);assert state['last_error']['stage']=='prefill' and state['last_error']['code']==504
    finally:app.close()
print('reasoning HTTP: fragmented markers/UTF-8, all endpoints, context budgets, no-answer, numeric records, faults, keepalive and prefill cancellation/recovery passed')
