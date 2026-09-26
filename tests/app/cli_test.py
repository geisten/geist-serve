#!/usr/bin/env python3
"""Isolated installed-client lifecycle and model reuse; never changes user config."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from http_test import App, ROOT

binary=Path(os.environ.get('GEIST_CLI_TEST_BINARY',ROOT/'geist'))
model=os.environ.get('GEIST_TEST_MODEL')
with tempfile.TemporaryDirectory(prefix='geist-cli-') as home:
    app=App(home,model=model)
    env=os.environ|{'GEIST_HOME':home,'GEIST_PORT':str(app.port)}
    if model: env['GEIST_MODEL']=str(model)
    def cli(*args,code=0):
        p=subprocess.run([str(binary),*args],env=env,capture_output=True,text=True,timeout=180)
        assert p.returncode==code,(args,p.returncode,p.stderr)
        return p.stdout
    try:
        cli('start')
        before=json.loads(cli('connection'))
        assert before['base_url'].endswith(f':{app.port}/v1')
        if model:
            app.wait(lambda s:s['ready'],timeout=60)
            before=json.loads(cli('connection'))
            result=json.loads(cli('test'))
            assert result['usage']['completion_tokens']>0
            assert json.loads(cli('connection'))['daemon_pid']==before['daemon_pid']
            cli('test-agent',code=3)
            c=json.loads(cli('config','continue'))
            assert c['models'][0]['capabilities']==[] and c['models'][0]['roles']==['chat']
            c=json.loads(cli('config','opencode'))
            assert c['agent']['geist-chat']['permission']=={'*':'deny'}
        # A user marker and secret survive restart; discovery represents the new owner.
        (Path(home)/'models'/'keep-me').write_text('preserved')
        key=before['api_key']
        cli('restart')
        after=json.loads(cli('connection'))
        assert after['api_key']==key and after['base_url']==before['base_url']
        if model:
            app.wait(lambda s:s['ready'],timeout=60)
            assert json.loads(cli('test'))['usage']['completion_tokens']>0
        assert (Path(home)/'models'/'keep-me').read_text()=='preserved'
        cli('stop')
        assert not (Path(home)/'connection.json').exists()
        cli('status',code=1)
        cli('start');cli('stop')
    finally:
        subprocess.run([str(binary),'stop'],env=env,capture_output=True,timeout=30)
        app.close()
print('CLI: shared service, chat/capability checks, private config, restart and preserved user files passed')
