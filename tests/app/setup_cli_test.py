#!/usr/bin/env python3
"""`geist setup` (#46): the first model from the terminal, never without consent.

Model-free: no terminal and no --yes means no download. With GEIST_TEST_MODEL
(the curated SmolLM2 GGUF) and build/geist-app-test, `geist setup --yes`
downloads it from a local fixture, loads it and runs one real generation.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http_test import App, ROOT

binary = Path(os.environ.get('GEIST_CLI_TEST_BINARY', ROOT/'geisten'))
model = os.environ.get('GEIST_TEST_MODEL')


def cli(home, port, *args, code=0):
    env = os.environ | {'GEIST_HOME': str(home), 'GEIST_PORT': str(port)}
    # A new session has no controlling terminal: /dev/tty cannot be opened, as in CI or a pipe.
    p = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True, timeout=600, start_new_session=True)
    assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
    return p.stdout + p.stderr


with tempfile.TemporaryDirectory(prefix='geist-setup-cli-') as temporary:
    home = Path(temporary)
    app = App(home)
    try:
        out = cli(home, app.port, 'setup')
        assert 'No model set up' in out and 'geisten setup' in out, out
        assert not list((home/'models').glob('*')), 'setup without consent touched the models folder'
        assert not app.status()['job_model']
        cli(home, app.port, 'setup', '--bogus', code=2)
        cli(home, app.port, 'setup', '--yes', 'extra', code=2)
    finally:
        app.close()

if model:
    fixture = Path(model)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Length', str(fixture.stat().st_size))
            self.end_headers()
            with fixture.open('rb') as stream:
                while chunk := stream.read(1 << 20):
                    self.wfile.write(chunk)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='geist-setup-cli-') as temporary:
        home = Path(temporary)
        (home/'selected').write_text('smollm2-360m')  # makes the fixture the recommendation
        env = os.environ | {'GEIST_TEST_MODEL_URL': f'http://127.0.0.1:{server.server_port}/fixture'}
        app = App(home, binary=ROOT/'build/geist-app-test', env=env, server=ROOT/'geistd')
        try:
            assert app.status()['recommendation']['id'] == 'smollm2-360m'
            out = cli(home, app.port, 'setup', '--yes')
            assert 'Model ready: SmolLM2 360M' in out and 'Test passed' in out, out
            assert (home/'models/smollm2-360m-instruct-q8_0.gguf').stat().st_size == fixture.stat().st_size
            state = app.status()
            assert state['ready'] and state['active_id'] == 'smollm2-360m'
            # --yes loads the model but is not the deliberate per-model preview choice.
            assert not next(m for m in state['models'] if m['id'] == 'smollm2-360m')['preview_accepted']
            out = cli(home, app.port, 'setup')
            assert 'already loaded: smollm2-360m' in out and 'Test passed' in out, out
        finally:
            app.close()
            server.shutdown()
print('geist setup: no consent, no download; ' + ('--yes downloads, loads and tests' if model else 'model run skipped (set GEIST_TEST_MODEL)'))
