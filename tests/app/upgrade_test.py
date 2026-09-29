#!/usr/bin/env python3
"""Exercise old/current/new services using real processes and isolated user data."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from http_test import App, ROOT

old = Path(os.environ['GEIST_OLD_APP'])
new = Path(os.environ['GEIST_NEW_APP'])
legacy = Path(os.environ['GEIST_LEGACY_APP'])
for binary, expected in [(old,0),(new,44),(legacy,42)]:
    with tempfile.TemporaryDirectory(prefix='geist-upgrade-') as home:
        app=App(home,binary=binary)
        env=os.environ|{'GEIST_HOME':home,'GEIST_PORT':str(app.port)}
        cli=ROOT/'geist'
        before=json.loads((Path(home)/'connection.json').read_text())
        marker=Path(home)/'models'/'keep-me';marker.write_text('preserved')
        try:
            result=subprocess.run([str(cli),'start'],env=env,capture_output=True,text=True,timeout=30)
            assert result.returncode==expected,(result.returncode,result.stderr)
            after=json.loads((Path(home)/'connection.json').read_text())
            assert marker.read_text()=='preserved'
            assert before['api_key']==after['api_key']
            if expected==0:
                assert before['pid'] != after['pid'], 'old process was silently reused'
                assert app.status()['version']=='0.5.17'
                # Opening again must reuse the current service.
                subprocess.run([str(cli),'start'],env=env,check=True,capture_output=True,timeout=30)
                assert json.loads((Path(home)/'connection.json').read_text())['pid']==after['pid']
            else:
                assert before['pid']==after['pid'], 'newer/legacy process was silently interrupted'
        finally:
            subprocess.run([str(cli),'stop'],env=env,capture_output=True,timeout=30)
            app.close()
print('upgrade: older service replaced, same service reused, newer/legacy services protected; models/key preserved')
