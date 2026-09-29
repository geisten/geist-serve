#!/usr/bin/env python3
"""Isolated real VS Code/Continue extension-host acceptance, no user settings."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from http_test import App, ROOT

os.umask(0o077)
code = Path(os.environ.get('GEIST_VSCODE_BINARY', ROOT/'build/vscode-test/Visual Studio Code.app/Contents/Resources/app/bin/code'))
extensions = ROOT/'build/vscode-test/extensions'
evidence = ROOT/'build/editor-acceptance'/f'vscode-{time.time_ns()}'
evidence.mkdir(parents=True)
with tempfile.TemporaryDirectory(prefix='geist-vscode-') as temp:
    root = Path(temp)
    workspace = root/'workspace'; workspace.mkdir()
    (workspace/'.continueignore').write_text('**/*\n')
    config = root/'continue'; config.mkdir()
    data = root/'data'; (data/'User').mkdir(parents=True)
    (data/'User/settings.json').write_text(json.dumps({
        'telemetry.telemetryLevel':'off', 'update.mode':'none',
        'extensions.autoUpdate':False, 'security.workspace.trust.enabled':False,
        'continue.telemetryEnabled':False, 'continue.enableTabAutocomplete':False,
        'continue.enableNextEdit':False, 'continue.pauseCodebaseIndexOnStart':True
    }))
    dev = root/'test-extension'; dev.mkdir()
    (dev/'package.json').write_text(json.dumps({'name':'geist-connection-test',
        'version':'0.0.1','publisher':'geisten','engines':{'vscode':'^1.90.0'},
        'main':'index.js','activationEvents':['*']}))
    (dev/'index.js').write_text('exports.activate = () => {};\n')
    app = App(root/'service', model=os.environ['GEIST_TEST_MODEL'])
    env = os.environ | {'GEIST_HOME':str(root/'service'), 'NODE_ENV':'test',
        'CONTINUE_GLOBAL_DIR':str(config), 'CONTINUE_METRICS_ENABLED':'false',
        'GEIST_VSCODE_RESULT':str(evidence/'summary.json')}
    try:
        app.wait(lambda s:s['ready'], timeout=60)
        before = json.loads(app.request('/app/connections')[1])['daemon_pid']
        content = subprocess.check_output([str(ROOT/'geist'),'config','continue'],env=env,text=True)
        (config/'config.yaml').write_text(content)
        p = subprocess.run([str(code),str(workspace),'--new-window','--wait',
            '--user-data-dir',str(data),'--extensions-dir',str(extensions),
            '--extensionDevelopmentPath',str(dev),
            '--extensionTestsPath',str(ROOT/'tests/app/vscode_client.cjs'),
            '--skip-welcome','--skip-release-notes','--disable-updates'],
            env=env,capture_output=True,text=True,timeout=180)
        (evidence/'stdout.log').write_text(p.stdout.replace(app.token,'[REDACTED]'))
        (evidence/'stderr.log').write_text(p.stderr.replace(app.token,'[REDACTED]'))
        assert p.returncode == 0, (p.returncode, p.stderr[-2000:],p.stdout[-2000:])
        result=json.loads((evidence/'summary.json').read_text())
        assert json.loads(app.request('/app/connections')[1])['daemon_pid'] == before
        print(json.dumps(result, indent=2))
    finally:
        app.close()
