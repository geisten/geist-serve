#!/usr/bin/env python3
"""#82: selection, active model and notices stay true through a failed switch,
a stop and a crash. Uses the protocol peer from background_download_test, no model."""
import json
import os
from pathlib import Path
import shlex
import signal
import sys
import tempfile
from http_test import App, ROOT
from background_download_test import model_catalog

binary = Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT/'build/geist-app-test'))

with tempfile.TemporaryDirectory(prefix='geist-state-') as temporary:
    root = Path(temporary)
    source = root/'fixture.gguf'
    source.write_bytes(b'x'*1048576)
    wrapper = root/'peer'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                       shlex.quote(str(ROOT/'tests/app/background_download_test.py')) + ' --peer "$@"\n')
    wrapper.chmod(0o700)
    home = root/'data'
    (home/'models').mkdir(parents=True)
    catalog = model_catalog(source)
    a, b = catalog['models']
    (home/'models'/a['file']).write_bytes(source.read_bytes())
    (home/'models'/b['file']).write_bytes(b'y'*1048576)  # right size, wrong content: verification fails
    (home/'catalog.json').write_text(json.dumps(catalog))
    (home/'selected').write_text(a['id'])
    app = App(home, binary=binary, server=wrapper, env={**os.environ, 'GEIST_PEER_GATE': str(home/'gate')})
    try:
        app.wait(lambda s: s['ready'] and s['active_id'] == a['id'] and not s['busy'], timeout=30)

        # A failed switch keeps the working model, also as the choice for the next launch.
        assert app.request('/app/select', {'id': b['id']})[0] == 202
        s = app.wait(lambda s: not s['busy'] and not s['phase'], timeout=30)
        assert s['ready'] and s['active_id'] == a['id'], s
        assert 'mismatch' in s['message'], s['message']
        assert (home/'selected').read_text() == a['id'], 'a failed switch changed the saved selection'

        # Stop leaves no notice about a state that no longer exists, and no active model.
        assert app.request('/app/stop', {})[0] == 200
        s = app.wait(lambda s: not s['ready'] and not s['loading'], timeout=30)
        assert s['message'] == '' and s['active_id'] == '' and s['execution']['notice'] == '', s

        # A crashed model process is not presented as the active or the connection model.
        assert app.request('/app/select', {'id': a['id']})[0] == 202
        s = app.wait(lambda s: s['ready'] and s['active_id'] == a['id'] and not s['busy'], timeout=30)
        os.kill(s['lifecycle']['pid'], signal.SIGKILL)
        s = app.wait(lambda s: not s['ready'] and not s['loading'], timeout=30)
        assert s['active_id'] == '' and s['active'] == '', s
        connection = json.loads(app.request('/app/connections')[1])
        assert connection['model'] == '' and not connection['ready'], connection
    finally:
        app.close()
print('state: failed switch keeps the selection; stop and crash leave no stale model or notice')
