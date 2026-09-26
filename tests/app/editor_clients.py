#!/usr/bin/env python3
"""Run installed clients in private, isolated configuration/workspace folders."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from http_test import App, ROOT

os.umask(0o077)
tools=ROOT/'build/client-tools/node_modules/.bin'
evidence=ROOT/'build/editor-acceptance'/f'run-{time.time_ns()}'
evidence.mkdir(parents=True)
with tempfile.TemporaryDirectory(prefix='geist-editor-') as home:
    root=Path(home); workspace=root/'workspace'; workspace.mkdir()
    app=App(root/'service',model=os.environ['GEIST_TEST_MODEL'])
    env=os.environ|{'GEIST_HOME':str(root/'service'),
        'CONTINUE_GLOBAL_DIR':str(root/'continue'), 'CONTINUE_CLI_ENABLE_TELEMETRY':'false',
        'CONTINUE_METRICS_ENABLED':'false', 'XDG_CONFIG_HOME':str(root/'config'),
        'XDG_DATA_HOME':str(root/'data'), 'XDG_CACHE_HOME':str(root/'cache'),
        'OPENCODE_CONFIG_DIR':str(root/'opencode'), 'OPENCODE_DISABLE_AUTOUPDATE':'true'}
    results={}
    try:
        app.wait(lambda s:s['ready'],timeout=60)
        for kind,name,args in [('opencode','opencode',['run','--pure','--print-logs','--log-level','DEBUG','--agent','geist-chat','--format','json','--title','Geist connection test','Say hello in one short sentence.']),
                               ('continue','cn',['--config',str(workspace/'config.yaml'),'--exclude','*','-p','--format','json','Say hello in one short sentence.'])]:
            if kind not in os.environ.get('GEIST_EDITOR_CLIENTS','opencode,continue').split(','): continue
            content=subprocess.check_output([str(ROOT/'geist'),'config',kind],env=env,text=True)
            config=workspace/('opencode.json' if kind=='opencode' else 'config.yaml')
            config.write_text(content)
            if kind=='opencode':
                env['OPENCODE_CONFIG']=str(config)
                args[1:1]=['--dir',str(workspace)]
            try:
                p=subprocess.run([str(tools/name),*args],env=env,cwd=workspace,capture_output=True,text=True,timeout=150)
                text=p.stdout.replace(app.token,'[REDACTED]'); errors=p.stderr.replace(app.token,'[REDACTED]')
                (evidence/f'{kind}.stdout').write_text(text)
                (evidence/f'{kind}.stderr').write_text(errors)
                results[kind]={'exit_code':p.returncode,'stdout_nonempty':bool(text.strip())}
                print(kind,p.returncode,text[-1600:],errors[-1600:],flush=True)
            except subprocess.TimeoutExpired as error:
                results[kind]={'timeout':True};print(kind,'timed out',flush=True)
        (evidence/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
        assert results and all(r.get('exit_code') == 0 and r.get('stdout_nonempty') for r in results.values()), results
    finally: app.close()
