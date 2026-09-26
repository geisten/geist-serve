#!/usr/bin/env python3
"""Real download/resume/checksum path against a local fixture, no internet.
Requires build/geist-app-test and GEIST_TEST_MODEL=the curated SmolLM2 GGUF.
The fixture URL override is compiled out of production builds.
"""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import tempfile
import threading
import time
from http_test import App, ROOT

fixture = Path(os.environ['GEIST_TEST_MODEL'])
assert fixture.stat().st_size == 386404992, 'This test needs the curated SmolLM2 fixture'
expected = '48ab3034d0dd401fbc721eb1df3217902fee7dab9078992d66431f09b7750201'
mode = 'slow'
offsets = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        offset = int(self.headers.get('Range', 'bytes=0-').split('=')[1].split('-')[0])
        offsets.append(offset)
        total = fixture.stat().st_size
        self.send_response(206 if offset else 200)
        self.send_header('Content-Length', str(total - offset))
        if offset:
            self.send_header('Content-Range', f'bytes {offset}-{total - 1}/{total}')
        self.end_headers()
        remaining = total - offset
        try:
            with fixture.open('rb') as stream:
                stream.seek(offset)
                while remaining:
                    chunk = stream.read(min(65536, remaining)) if mode != 'corrupt' else bytes(min(65536, remaining))
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
                    if mode == 'slow':
                        time.sleep(.005)
        except (BrokenPipeError, ConnectionResetError):
            pass


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
env = dict(os.environ, GEIST_TEST_MODEL_URL=f'http://127.0.0.1:{server.server_port}/fixture')
try:
    with tempfile.TemporaryDirectory(prefix='geist-download-') as home:
        app = App(home, binary=ROOT / 'build/geist-app-test', env=env)
        target = Path(home) / 'models/smollm2-360m-instruct-q8_0.gguf'
        part = target.with_suffix('.gguf.part')
        try:
            assert app.request('/app/download', {'id': 'smollm2-360m'})[0] == 202
            app.wait(lambda s: s['received'] > 524288)
            assert app.request('/app/download', {'id': 'bitnet-2b'})[0] == 409
            assert app.request('/app/cancel', {})[0] == 200
            app.wait(lambda s: not s['busy'])
            assert part.exists() and 0 < part.stat().st_size < fixture.stat().st_size
            assert not target.exists()
            saved = part.stat().st_size
        finally:
            app.close()
        mode = 'valid'
        app = App(home, binary=ROOT / 'build/geist-app-test', env=env)
        try:
            assert app.request('/app/download', {'id': 'smollm2-360m'})[0] == 202
            app.wait(lambda s: not s['busy'], timeout=90)
            assert saved in offsets, offsets
            assert target.exists() and not part.exists()
            digest = hashlib.sha256()
            with target.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1048576), b''):
                    digest.update(chunk)
            assert digest.hexdigest() == expected
            assert (Path(home) / 'selected').read_text() == 'smollm2-360m'
            print('download: cancellation, concurrent-action rejection, resume across launch, SHA and atomic placement passed')
        finally:
            app.close()
    mode = 'corrupt'
    with tempfile.TemporaryDirectory(prefix='geist-corrupt-') as home:
        app = App(home, binary=ROOT / 'build/geist-app-test', env=env)
        try:
            assert app.request('/app/download', {'id': 'smollm2-360m'})[0] == 202
            state = app.wait(lambda s: not s['busy'], timeout=90)
            assert 'mismatch' in state['message'], state
            assert not list((Path(home) / 'models').glob('*.gguf*'))
            assert not (Path(home) / 'selected').exists()
            assert not state['ready']
            print('download: correctly sized corrupt model discarded and never selected or started')
        finally:
            app.close()
finally:
    server.shutdown(); server.server_close()
