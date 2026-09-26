#!/usr/bin/env python3
"""Actual shared-daemon HTTP acceptance; negative agent tests are not agent approval."""
import http.client
import json
import os
from pathlib import Path
import stat
import tempfile
from http_test import App, ROOT

binary = Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT/'geist-app'))
model = os.environ.get('GEIST_TEST_MODEL')
base = {'model':'custom','messages':[{'role':'system','content':'Answer briefly.'},
        {'role':'user','content':'Name one color.'}], 'max_tokens':32}
with tempfile.TemporaryDirectory(prefix='geist-compat-') as home:
    app=App(home,model=model,binary=binary)
    try:
        assert app.request('/v1/models',auth=False)[0]==401
        assert app.request('/v1/models',headers={'Origin':'https://evil.example'})[0]==403
        for extra in [{'tools':[{'type':'function','function':{'name':'execute'}}]},
                      {'response_format':{'type':'json_object'}}, {'tool_choice':'auto'}, {'n':2}]:
            code,body,_=app.request('/v1/chat/completions',base|extra)
            assert code==422 and 'message' in json.loads(body)['error'],body
        for extra in [{'messages':[]},{'max_tokens':-1},{'max_tokens':1025},{'temperature':3},
                      {'stream':'true'},{'stream_options':{'include_usage':'yes'}},
                      {'messages':[{'role':'user','content':'hidden\u0000suffix'}]},
                      {'messages':[{'role':'tool','content':'output'}]}]:
            assert app.request('/v1/chat/completions',base|extra)[0]==400
        for suffix in [',"stream":truejunk', ',"max_tokens":1xyz', ',"max_tokens":01', ',"model":"duplicate"']:
            data=(json.dumps({'model':'custom','messages':[{'role':'user','content':'hello'}]})[:-1]+suffix+'}').encode()
            response=app.raw((f'POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:{app.port}\r\nAuthorization: Bearer {app.token}\r\nContent-Length: {len(data)}\r\n\r\n').encode()+data)
            assert response.startswith(b'HTTP/1.1 400'),response
        assert stat.S_IMODE((Path(home)/'api-key').stat().st_mode)==0o600
        descriptor=json.loads((Path(home)/'connection.json').read_text())
        assert descriptor['api_key']==app.token and descriptor['port']==app.port
        assert stat.S_IMODE((Path(home)/'connection.json').stat().st_mode)==0o600
        if model:
            app.wait(lambda s:s['ready'],timeout=60)
            before=json.loads(app.request('/app/connections')[1])
            assert before['daemon_pid']>0 and not before['tools']
            models=json.loads(app.request('/v1/models')[1])['data']
            assert [m['id'] for m in models]==['custom']
            assert app.request('/v1/chat/completions',base|{'model':'not-loaded'})[0]==404
            code,body,_=app.request('/v1/chat/completions',base)
            result=json.loads(body)
            assert code==200 and result['choices'][0]['message']['content'],body
            assert result['usage']['completion_tokens']>0
            code,body,headers=app.request('/v1/chat/completions',base|{'stream':True,'stream_options':{'include_usage':True}})
            events=[line[6:] for line in body.decode().splitlines() if line.startswith('data: ')]
            assert code==200 and headers['Content-Type']=='text/event-stream' and events[-1]=='[DONE]',body
            parsed=[json.loads(e) for e in events[:-1]]
            assert parsed[-1]['usage']['completion_tokens']>0
            assert any(x['choices'] and x['choices'][0].get('finish_reason') in ('stop','length') for x in parsed)
            assert json.loads(app.request('/app/connections')[1])['daemon_pid']==before['daemon_pid']
            # Text parts, history, and another independent browser task reuse that PID.
            history=base|{'messages':[{'role':'user','content':[{'type':'text','text':'Say hello.'}]}]}
            assert app.request('/v1/chat/completions',history)[0]==200
            assert app.request('/app/generate',{'prompt':'Say hello.','experimental':True})[0]==200
            assert json.loads(app.request('/app/connections')[1])['daemon_pid']==before['daemon_pid']
            # Disconnect cancels the owned session. The next client must remain usable.
            conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=30)
            conn.request('POST','/v1/chat/completions',json.dumps(base|{'stream':True,'max_tokens':1024}),
                         {'Authorization':'Bearer '+app.token})
            response=conn.getresponse(); assert response.status==200
            response.readline()
            assert app.request('/v1/chat/completions',base)[0]==429
            response.close();conn.close()
            app.wait(lambda s:not s['busy'],timeout=15)
            assert app.request('/v1/chat/completions',base)[0]==200
            assert app.request('/v1/chat/completions',base|{'messages':[{'role':'user','content':' xy'*7000}]})[0]==400
            assert not app.status()['busy']
        old_token,port=app.token,app.port
    finally: app.close()
    assert not (Path(home)/'connection.json').exists()
    restarted=App(home,binary=binary,port=port)
    try: assert restarted.token==old_token
    finally: restarted.close()
print('shared API: auth, capability rejection, private stable credentials and restart passed')
print('shared API real inference: '+('passed; browser and API used the same daemon PID' if model else 'SKIPPED; set GEIST_TEST_MODEL'))
