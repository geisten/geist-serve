#!/usr/bin/env python3
"""#143: the public API also on <home>/api.sock: 0600, same key, stale socket replaced."""
import http.client
import json
import os
from pathlib import Path
import socket
import stat
import tempfile
from http_test import App


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=20)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(20)
        self.sock.connect(self.path)


def over_socket(path, route, key=None, host='localhost'):
    connection = UnixConnection(path)
    headers = {'Host': host} | ({'Authorization': f'Bearer {key}'} if key else {})
    connection.request('GET', route, headers=headers)
    response = connection.getresponse()
    body = response.read()
    connection.close()
    return response.status, body


def socket_of(app):
    code, body, _ = app.request('/app/connections')
    assert code == 200, body
    return json.loads(body)['socket']


# Short folder: a socket path is limited to about 104 bytes.
with tempfile.TemporaryDirectory(prefix='gs-', dir='/tmp') as home:
    path = Path(home, 'api.sock')
    stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    stale.bind(str(path))
    stale.close()  # left behind as after a crash
    app = App(home)
    try:
        assert socket_of(app) == str(path) == app.status()['api_socket'], socket_of(app)
        mode = path.lstat().st_mode
        assert stat.S_ISSOCK(mode) and stat.S_IMODE(mode) == 0o600 and path.lstat().st_uid == os.getuid(), oct(mode)
        code, body = over_socket(str(path), '/v1/models', app.token)
        assert code == 200 and json.loads(body)['object'] == 'list', (code, body)
        assert over_socket(str(path), '/v1/models')[0] == 401, 'the key is still required'
        assert over_socket(str(path), '/v1/models', app.token, host='example.com')[0] == 200, 'no Host check: browsers cannot reach the socket'
        assert app.request('/v1/models', headers={'Host': 'example.com'})[0] == 403, 'TCP keeps its Host check'
    finally:
        app.close()
    assert not path.exists(), 'the socket is removed on exit'

    path.write_text('not a socket')  # never removed: TCP only
    app = App(home)
    try:
        assert socket_of(app) is None and app.status()['api_socket'] is None and path.read_text() == 'not a socket'
        assert app.request('/v1/models')[0] == 200
    finally:
        app.close()

with tempfile.TemporaryDirectory(prefix='gs-' + 'x' * 110, dir='/tmp') as home:
    app = App(home)
    try:
        assert socket_of(app) is None, 'a path too long for a socket leaves TCP only'
        assert app.request('/v1/models')[0] == 200
    finally:
        app.close()
print('api socket: 0600 socket with key check, stale socket replaced, foreign file kept, long path TCP only, removed on exit')
