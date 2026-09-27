#!/usr/bin/env python3
"""Session-chat contract through the real authenticated app and resident daemon."""
import json
import os
import tempfile
from pathlib import Path
from http_test import App, ROOT

model = os.environ.get('GEIST_TEST_MODEL')
binary = Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT / 'geist-app'))
base = dict(model='custom', prompt='Which word did I ask you to remember?',
            messages=[{'role':'user','content':'Remember the word lighthouse.'},
                      {'role':'assistant','content':'I will remember lighthouse.'},
                      {'role':'user','content':'Which word did I ask you to remember?'}],
            task='freeform', task_version='1.0.0', language='en', max_tokens=32, experimental=True)
with tempfile.TemporaryDirectory(prefix='geist-session-chat-') as home:
    app = App(home, model=model, binary=binary)
    try:
        assert app.request('/app/generate',base,auth=False)[0] == 403
        for extra in [dict(messages=[]), dict(messages=base['messages'][:-1]),
                      dict(messages=[{'role':'system','content':base['prompt']}]),
                      dict(messages=[{'role':'user','content':'different'}]),
                      dict(messages=[{'role':'user','content':'hidden\u0000suffix'}]),
                      dict(messages=[{'role':'assistant','content':base['prompt']}]),
                      dict(task='summary'), dict(benchmark=True), dict(max_tokens=1025),
                      dict(messages=[{'role':'user','content':'x'},{'role':'assistant','content':''},base['messages'][-1]])]:
            code, body, _ = app.request('/app/generate', base | extra)
            assert code == 400, (extra, code, body)
        if model:
            app.wait(lambda s:s['ready'],timeout=60)
            assert app.request('/app/generate',base|{'experimental':False})[0] == 409
            assert app.request('/app/generate',base|{'model':'changed'})[0] == 409
            code, body, _ = app.request('/app/generate',base)
            events = [json.loads(line) for line in body.decode().splitlines()]
            answer = ''.join(e.get('response','') for e in events)
            assert code == 200 and answer and events[-1]['done'], body
            # Token accounting proves the provided turns reached the daemon;
            # wording/recall quality is observed, not a substitute for a quality evaluation.
            assert events[-1]['prompt_eval_count'] > 20
            code, body, _ = app.request('/app/generate',base|{'max_tokens':1})
            assert code == 200 and json.loads(body.decode().splitlines()[-1])['limited'] is True
            oversized = base | {'prompt':' x'*5900, 'messages':[{'role':'user','content':' x'*5900}]}
            code, body, _ = app.request('/app/generate',oversized)
            assert code == 400 and b'context' in body, body
            assert not app.status()['busy']
            print('Real multi-turn response:', repr(answer))
    finally:
        app.close()
print('session chat: role/order/current-input/consent/model/limits passed; real inference ' + ('passed' if model else 'SKIPPED'))
