#!/usr/bin/env python3
"""#142: every Connect-page snippet runs unchanged against a real service.

Snippets come from web/snippets.js (via node), exactly as the page copies them.
A language runs when its toolchain and SDK are present; GEIST_SNIPPETS_REQUIRED
(comma-separated) turns a missing one into a failure, as CI does.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from http_test import App, ROOT

required = {k for k in os.environ.get('GEIST_SNIPPETS_REQUIRED', 'terminal').split(',') if k}
modules = Path(os.environ.get('GEIST_SNIPPETS_NODE_MODULES', ROOT / 'build/client-tools/node_modules'))


def has_python_sdk():
    return subprocess.run(['python3', '-c', 'import openai'], capture_output=True).returncode == 0


# kind → (file name, command, available?)
languages = {
    'terminal': ('hello.sh', ['sh', 'hello.sh'], lambda: shutil.which('curl')),
    'python': ('hello.py', ['python3', 'hello.py'], has_python_sdk),
    'javascript': ('hello.mjs', ['node', 'hello.mjs'], lambda: shutil.which('node') and (modules / 'openai').is_dir()),
    'go': ('main.go', ['sh', '-c', 'go mod init hello >/dev/null 2>&1 && go get github.com/openai/openai-go >/dev/null 2>&1 && go run .'], lambda: shutil.which('go')),
    'java': ('Hello.java', ['java', 'Hello.java'], lambda: shutil.which('java')),
}

with tempfile.TemporaryDirectory(prefix='geist-snippets-') as home:
    app = App(home, model=os.environ['GEIST_TEST_MODEL'])
    try:
        app.wait(lambda s: s['ready'], timeout=60)
        connection = json.loads(app.request('/app/connections')[1])
        base = f'http://127.0.0.1:{app.port}/v1'
        code = 'const s=require(process.argv[1]);process.stdout.write(JSON.stringify(Object.fromEntries(Object.entries(s).map(([k,f])=>[k,f(...process.argv.slice(2))]))))'
        snippets = json.loads(subprocess.check_output(['node', '-e', code, str(ROOT / 'web/snippets.js'), base, connection['api_key'], connection['model']]))
        assert set(snippets) == set(languages), snippets.keys()
        ran, skipped = [], []
        for kind, (name, command, available) in languages.items():
            if not available():
                assert kind not in required, f'{kind}: toolchain or SDK missing'
                skipped.append(kind)
                continue
            with tempfile.TemporaryDirectory(prefix=f'geist-{kind}-') as work:
                Path(work, name).write_text(snippets[kind])
                if kind == 'javascript':
                    Path(work, 'node_modules').symlink_to(modules)
                result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=600)
                assert result.returncode == 0, (kind, result.stdout[-2000:], result.stderr[-2000:])
                text = result.stdout.strip()
                if kind in ('terminal', 'java'):
                    text = json.loads(text)['choices'][0]['message']['content'].strip()
                assert text, (kind, result.stdout)
                ran.append(kind)
    finally:
        app.close()
print(f"snippets: ran {', '.join(ran)}" + (f"; skipped {', '.join(skipped)} (toolchain or SDK missing)" if skipped else ''))
