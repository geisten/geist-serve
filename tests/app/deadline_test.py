#!/usr/bin/env python3
"""Trickling headers and bodies must not exhaust every HTTP worker forever."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import select
import socket
import tempfile
import time
from http_test import App, ROOT

binary = Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT / 'geist-app'))
with tempfile.TemporaryDirectory(prefix='geist-deadline-') as home:
    app = App(home, binary=binary)
    try:
        def trickle(body):
            with socket.create_connection(('127.0.0.1', app.port), timeout=10) as sock:
                if body:
                    sock.sendall((f'POST /app/generate HTTP/1.1\r\nHost: localhost:{app.port}\r\n'
                                  'Content-Length: 10000\r\n\r\n').encode())
                else:
                    sock.sendall(b'GET /health HTTP/1.1\r\nX-Slow: ')
                start = time.monotonic()
                while time.monotonic() - start < 10:
                    sock.sendall(b'x')
                    if select.select([sock], [], [], .2)[0]:
                        reply = sock.recv(4096)
                        assert b' 408 ' in reply, reply
                        assert time.monotonic() - start < 8
                        return
                raise AssertionError('trickling request outlived its total deadline')
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(trickle, [False, True] * 4))
        assert app.request('/health', auth=False)[0] == 200
        assert app.status()['busy'] is False
        print('request deadline: all eight trickling header/body workers released and app recovered')
    finally:
        app.close()
